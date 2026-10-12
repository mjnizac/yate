-- Dunes: rounded sand, no water.
--
-- Billow noise is |n| folded, so it is all crests and no sharp valleys, which is the right shape
-- for wind-blown sand. There is no anisotropic noise in the engine, so these are barchan-ish lumps
-- rather than directional ridges.
--
--   terrain_viewer --script assets/scripts/terrains/dunes.lua

local function main(params)

    local field = Noises.Billow{
        frequency = 0.0009, octaves = 2, lacunarity = 2.3, persistence = 0.35,
        amplitude = 1.0, seed = params.seed,
    }
    -- A long undulation underneath, so the dune field has highs and lows of its own.
    local sheet = Noises.fBm{
        frequency = 0.00025, octaves = 2, amplitude = 140.0, seed = params.seed + 13,
    }

    -- Power above 1 pulls the field towards zero, which deepens the troughs between crests.
    --
    -- The billow is built at amplitude 1 and scaled to metres *after* the curve, which matters more
    -- than it looks: Power is `sign(v) * |v|^e`, so on a field already in metres it is an enormous
    -- multiplier. Scaled first, at amplitude 95 and exponent 1.3 the output would reach 95^1.3, far past the dune height asked for, and the
    -- dunes come out as black spikes. Curves.Power only means what you want on a field in [-1, 1].
    local shaped = Curves.Power{ input = field.value, exponent = 1.3 }

    local sand = shaped * 95.0 + sheet.value

    -- Sand stands at a low angle: a low talus and plenty of iterations is what a dune is.
    local settled = Erosion.Thermal{
        input = sand, iterations = 24, talus = math.tan(math.rad(18.0)), strength = 0.4,
    }

    return {
        height  = { value = settled, range = { -300, 300 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
    }
end

return main
