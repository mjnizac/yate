-- Lakes and rivers: a drained landscape, its rivers carved in and its lakes and rivers as outputs.
--
-- Rolling hills falling to the west, with two closed basins. `Erosion.StreamPower` cuts the valleys on a
-- 256 grid; `Water.Drain` breaches the shallow pits it leaves, keeps the two basins as lakes at their
-- spill level and finds the rivers; `Water.Apply` carves the rivers into the terrain at full resolution.
-- The west falls under the sea at 0. The viewer's water draws the sea, `lakes` and `rivers`:
--
--   terrain_viewer --script assets/scripts/terrains/lakes_rivers.lua --water
--
-- Parameters: `relief` (metres of the hills), `lake` (metres the basins sink under their lowest point), `channel` (square
-- metres a river drains before it is drawn), `stage` (1 the drained grid only, 2 with rivers and detail).

local function main(params)
    local seed    = params.seed or 0
    local relief  = params.relief or 200.0
    local lake    = params.lake or 25.0
    local channel = params.channel or 2e6
    local stage   = params.stage or 2

    -- The base on a 256 grid of 50 m, +-6.4 km: a westward tilt, hills from a few sinusoids whose phases
    -- the seed moves, and two Gaussian basins deep enough to hold water.
    local n, cell, origin = 256, 50.0, -6400.0
    local phase = {}
    for i = 1, 6 do
        phase[i] = ((seed * 7919 + i * 104729) % 6283) / 1000.0
    end
    local basins = { { -1800.0, 1500.0, 1300.0 }, { 2600.0, -2200.0, 900.0 } }
    -- A few metres of white noise per cell: on a smooth surface the drainage runs down parallel D8 lines,
    -- and a little roughness is what lets the channels meet and branch.
    local state = 12345 + seed * 7919
    local function random()
        state = (state * 1103515245 + 12345) % 2147483648
        return state / 2147483648
    end
    local start, still = {}, {}
    for zi = 0, n - 1 do
        for xi = 0, n - 1 do
            local x, z = origin + xi * cell, origin + zi * cell
            local h = 0.05 * (x - origin)
                + relief * (0.45 * math.sin(x / 1900.0 + phase[1]) * math.cos(z / 2300.0 + phase[2])
                    + 0.30 * math.sin(z / 1700.0 + 0.3 * math.sin(x / 2100.0) + phase[3])
                    + 0.25 * math.cos((x - 0.6 * z) / 900.0 + phase[4]))
            start[#start + 1] = h + 4.0 * random()
            still[#still + 1] = 0.0
        end
    end
    -- The closed troughs of the hills breached first, up to 40 m deep: `Erosion.StreamPower`
    -- fills what it cannot drain and cuts straight D8 spokes across the fill.
    local open = Hydrology.Drain{ size = { n, n }, cell = cell, height = start, breach = 40.0 }
    local carved = Erosion.StreamPower{
        size = { n, n }, cell = cell, height = open.height, uplift = still, iterations = 120, k = 0.012,
        m = 0.5, talus = 0.7, diffusion = 0.01 * cell * cell, refresh = 4,
    }
    -- The basins after the erosion, which fills every depression it routes through: dug into the carved
    -- valleys, as a glacier or a fault would, they are what `Water.Drain` keeps as lakes. Each blends the
    -- ground towards a floor `lake` metres under its lowest point, so whatever the slope around it, its
    -- rim is the ground as it was and it holds water up to the lowest notch in that rim.
    for _, b in ipairs(basins) do
        local lowest = math.huge
        for i = 1, #carved do
            local x, z = origin + ((i - 1) % n) * cell, origin + ((i - 1) // n) * cell
            if (x - b[1]) ^ 2 + (z - b[2]) ^ 2 < b[3] * b[3] then
                lowest = math.min(lowest, carved[i])
            end
        end
        for i = 1, #carved do
            local x, z = origin + ((i - 1) % n) * cell, origin + ((i - 1) // n) * cell
            local a = math.atan(z - b[2], x - b[1])
            -- A lobed outline, not a disc: the radius varies by a third around the basin.
            local radius = b[3] * (1.0 + 0.22 * math.sin(3.0 * a + phase[5]) + 0.12 * math.sin(5.0 * a + phase[6]))
            local u = math.max(0.0, math.min(1.0, 2.0 - 2.0 * math.sqrt((x - b[1]) ^ 2 + (z - b[2]) ^ 2) / radius))
            u = u * u * (3.0 - 2.0 * u)
            carved[i] = carved[i] + (lowest - lake - carved[i]) * u
        end
    end
    -- West of the tilt the ground falls under 0, which is the sea the rivers reach.
    local water = Water.Drain{
        values = carved, size = { n, n }, origin = { origin, origin }, cell = cell, sea_level = 0.0,
        channel_area = channel,
    }

    local ground = water.height
    if stage >= 2 then
        -- Detail well under the slope of the hills, so it adds texture and no new pits.
        local detail = Noises.fBm{
            frequency = 0.004, octaves = 5, persistence = 0.5, amplitude = 6.0, seed = seed + 3,
        }
        ground = ground + detail.value
    end
    local wet = stage >= 2 and Water.Apply{ height = ground, water = water } or { height = ground, lakes = water.lake }

    local out = {
        height  = { value = wet.height, range = { -200, 900 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = wet.height } } },
        lakes   = { value = wet.lakes, range = { -200, 900 } },
    }
    if wet.rivers ~= nil then
        out.rivers = { value = wet.rivers, range = { -400, 900 } }
    end
    return out
end

return main
