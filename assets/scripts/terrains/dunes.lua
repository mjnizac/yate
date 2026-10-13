-- Dunes: a sand sea of crescent chains on one wind, with smaller dunes on their backs.
--
--   terrain_viewer --script assets/scripts/terrains/dunes.lua
--
-- `Dunes.Field` (prelude, the dune macro) does the work; see docs/lua_reference.md. Parameters:
-- `wind` (degrees), `wavelength` (metres between crests), `height` (metres), `style` ("transverse" or
-- "star").

local function main(params)
    return Dunes.Field{
        seed       = params.seed,
        wind       = params.wind,
        wavelength = params.wavelength,
        height     = params.height,
        style      = params.style,
        sand       = params.sand,
        variety    = params.variety,
        cell       = params.cell,
        cells      = params.cells,
        iterations = params.iterations,
    }
end

return main
