-- Case: basic_fbm
--
-- The parameters in case.json drive the fBm kernel directly until the Lua runtime lands
-- (milestone 6). This file is the script that will replace them, and it is already hashed into the
-- metadata sidecar so a change to it is visible in the golden data.

local function main(params)
    local noise = Noises.fBm{
        kind       = "simplex",
        frequency  = 0.004,
        octaves    = 4,
        lacunarity  = 2.0,
        persistence = 0.5,
        seed        = params.seed,
    }

    local height = noise.value * 900.0 + 700.0

    return {
        height  = { value = height, range = { -200, 1800 } },
        normals = { value = Terrain.Normals{ input = noise.gradient } },
    }
end

return main
