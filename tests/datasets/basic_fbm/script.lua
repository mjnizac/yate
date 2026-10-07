-- Case: basic_fbm
--
-- Fractal simplex heightmap with its analytic normals. The numbers live here rather than in
-- case.json, so the script is the whole definition of the case and its hash in the metadata sidecar
-- covers every parameter.

local function main()
    local noise = Noises.fBm{
        kind        = "simplex",
        frequency   = 0.004,
        octaves     = 4,
        lacunarity  = 2.0,
        persistence = 0.5,
        amplitude   = 900.0,
        offset      = 700.0,
    }

    return {
        height  = { value = noise.value, range = { -200, 1800 } },
        normals = { value = Terrain.Normals{ input = noise.gradient } },
    }
end

return main
