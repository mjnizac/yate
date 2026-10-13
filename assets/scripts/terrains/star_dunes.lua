-- Star dunes: three winds at 120 degrees, so every dune peaks with arms in three directions.
--
--   terrain_viewer --script assets/scripts/terrains/star_dunes.lua

local function main(params)
    return Dunes.Field{
        seed = params.seed, style = "star", wind = params.wind or 10.0,
        wavelength = params.wavelength or 900.0, height = params.height or 110.0, flats = 0.35,
    }
end

return main
