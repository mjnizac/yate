-- Case: blended_r2
--
-- The first case that is a real graph rather than one noise node: two noise sources, a slope mask,
-- a blend, a curve, arithmetic on handles and analytic normals. It is what covers the parts of the
-- compiler that only a multi-node graph reaches -- buffer reuse, per-channel liveness and a shared
-- gradient feeding two consumers -- and it reads a user parameter, so the --param path is covered too.

local function main(params)
    local amplitude = params.amplitude or 400.0

    local base = Noises.fBm{
        frequency   = 0.003,
        octaves     = 4,
        lacunarity  = 2.0,
        persistence = 0.5,
        amplitude   = 1.0,
    }

    local peaks = Noises.Ridged{
        frequency = 0.012,
        octaves   = 3,
        amplitude = 1.0,
        seed      = 11,
    }

    -- The gradient feeds the mask and the normals, so one dispatch serves both consumers and the
    -- compiler has to keep the gradient buffer alive across them.
    local mask = Masks.Slope{ input = base.gradient, min = 0.15, max = 0.55 }

    local blended = Combine.Blend{
        a    = base.value,
        b    = Curves.Smoothstep{ input = peaks.value, min = -0.2, max = 0.8 },
        mask = mask,
    }

    return {
        height  = { value = blended * amplitude + 200.0,
                    range = { 200.0 - amplitude, 200.0 + amplitude } },
        mask    = { value = mask, range = { 0, 1 } },
        normals = { value = Terrain.Normals{ input = base.gradient } },
    }
end

return main
