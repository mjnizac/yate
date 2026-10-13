-- Dunes: a sand sea of dune chains on one wind, with great dunes under them and small ones on top.
--
--   terrain_viewer --script assets/scripts/terrains/dunes.lua
--
-- `Dunes.Field` (prelude 40_dunes.lua, the dune macro) does the work; see docs/lua_reference.md. Every
-- key is forwarded unchanged, so `--param key=value` reaches the macro, and a key left unset stays nil
-- so the macro's own defaults apply. Which keys a style takes is the macro's to say: one that belongs to
-- another style is an error, not silently ignored.
--
--   all styles   wind (degrees), repose (degrees), swell (metres), style ("procedural" (default),
--                "simulated" or "star")
--   procedural   wavelength, height, sinuosity, windward_angle, draa_wavelength, draa_height,
--                small_wavelength, small_height, region, margin
--   simulated    sand, variety, cell, cells, iterations, smooth, relief, small_wavelength,
--                small_height, draa_wavelength, draa_height
--   star         wavelength, height, sinuosity, back, flats, small

local KEYS = {
    "wind", "style", "repose", "swell",
    "sand", "variety", "cell", "cells", "iterations", "smooth", "relief",
    "small_wavelength", "small_height", "draa_wavelength", "draa_height",
    "wavelength", "height", "sinuosity", "back", "flats", "small", "windward_angle", "region", "margin",
}

local function main(params)
    local t = { seed = params.seed }
    for _, key in ipairs(KEYS) do
        t[key] = params[key]
    end
    -- Not `return Dunes.Field(t)`: a tail call drops this frame, and an error would then name a line of
    -- the prelude instead of this script.
    local field = Dunes.Field(t)
    return field
end

return main
