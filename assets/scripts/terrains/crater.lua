-- Crater: a raised rim around a flat caldera, on a noisy plain.
--
-- The rim is a ring: one smoothstep rising at the inner radius, another falling past the rim, and
-- the product is a band. Everything positional goes through Terrain.Coords, so it is in samples and
-- is multiplied by params.resolution.
--
--   terrain_viewer --script assets/scripts/terrains/crater.lua

local function main(params)
    local r      = params.resolution
    local bowl   = params.bowl or 420.0     -- caldera radius, metres
    local rim    = params.rim or 650.0      -- where the rim crest sits
    local skirt  = params.skirt or 1150.0    -- where the ejecta blanket fades out

    local p  = Terrain.Coords{ domain = 2 }
    local dx = p.x * r
    local dz = p.y * r
    local d2 = dx * dx + dz * dz

    -- Rising edge at the caldera wall, falling edge past the crest: the product is the rim band.
    local rising  = Curves.Smoothstep{ input = d2, min = bowl * bowl, max = rim * rim }
    local falling = Curves.Smoothstep{ input = -d2, min = -(skirt * skirt), max = -(rim * rim) }
    local ring    = rising * falling

    -- The floor is depressed inside the caldera and flat outside it.
    local inside = Curves.Smoothstep{ input = -d2, min = -(rim * rim), max = -(bowl * bowl) }

    local rough = Noises.fBm{
        frequency = 0.0035, octaves = 5, persistence = 0.4, amplitude = 22.0, seed = params.seed,
    }
    local plain = Noises.fBm{
        frequency = 0.0009, octaves = 3, amplitude = 55.0, seed = params.seed + 4,
    }

    local height = plain.value + ring * 230.0 - inside * 170.0 + rough.value * (1.0 - inside * 0.7)

    local settled = Erosion.Thermal{
        input = height, iterations = 14, talus = r * math.tan(math.rad(36.0)), strength = 0.3,
    }

    return {
        height  = { value = settled, range = { -350, 450 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
    }
end

return main
