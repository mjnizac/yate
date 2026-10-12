-- Volcanic island: a truncated cone with a crater, a parasitic cone, gullies and a sea floor.
--
-- Built from shapes rather than from noise thresholds. One warped position is shared by every part, so
-- they bend together and the crater stays at the top of the cone. The cone is a linear profile of a
-- circle raised to a power, which makes the flanks concave: steep near the summit and shallow at the
-- shore, the way a stratovolcano is. It is sized so that the *rim* of the crater reaches `peak`, and the
-- crater is a flat floor cut into it with a smooth minimum against a ceiling that only exists inside the
-- crater's mask.
--
--   terrain_viewer --script assets/scripts/shapes/volcanic_island.lua --water
--
-- Parameters (metres): radius is the foot of the cone, peak the height of the rim above the sea, crater
-- the crater's radius and depth how far its floor sits below the rim, floor the sea floor, warp how far
-- the outline wanders.

local function main(params)
    local radius = params.radius or 1500.0
    local peak   = params.peak or 620.0
    local crater = params.crater or 280.0
    local depth  = params.depth or 170.0
    local floor  = params.floor or -120.0
    local warp   = params.warp or 150.0
    local power  = 1.5

    local position = Shapes.Warp{ amplitude = warp, frequency = 0.0014, octaves = 3, seed = params.seed }

    -- The cone. `rise` is 0 at the foot and 1 at the apex. It is scaled so that the point at the
    -- crater's radius lands on `peak`: the apex above it is cut away, so it does not set the height.
    local at_rim = ((radius - crater) / radius) ^ power
    local foot   = Shapes.Circle{ radius = radius, position = position }
    local rise   = Curves.Profile{ input = foot, kind = "linear", width = radius }
    local cone   = Curves.Power{ input = rise, exponent = power } * ((peak - floor) / at_rim)

    -- A parasitic cone on the flank, which is what keeps one cone from looking like a drawing of one.
    local vent = Landforms.Cone{
        at = { 620, -380 }, radius = 300, height = 170, profile = "smooth", position = position,
    }

    -- The crater: a ceiling at the floor's height inside the crater's mask and far above it outside.
    -- Taking the smooth minimum with the land cuts the summit down to a flat floor with a rounded lip.
    local inside  = Shapes.Mask{
        shape = Shapes.Circle{ radius = crater, position = position }, feather = 20,
    }
    local ceiling = (peak - depth) + (1.0 - inside) * 4000.0
    local summit  = Combine.SmoothMin{ a = floor + cone, b = ceiling, k = 40.0 }

    -- Gullies: ridged noise, kept to the flanks by a feathered mask so the shore and the sea floor
    -- stay smooth.
    local flank = Shapes.Mask{
        shape   = Shapes.Circle{ radius = radius * 0.85, position = position },
        feather = radius * 0.25,
    }
    local gullies = Noises.Ridged{
        frequency = 0.0035, octaves = 4, amplitude = 45.0, seed = params.seed + 1,
    }

    local land = Combine.SmoothMax{ a = summit, b = floor + vent, k = 60.0 } + gullies.value * flank

    local carved = Erosion.Hydraulic{
        input = land, iterations = 20, rain = 0.4, capacity = 24.0,
    }
    -- One-sample ripple from the hydraulic transfers; see docs/lua_reference.md.
    local smoothed = Filters.Blur{ input = carved, radius = 1 }
    local settled  = Erosion.Thermal{
        input = smoothed, iterations = 8, talus = math.tan(math.rad(40.0)), strength = 0.3,
    }

    -- Nothing below the shelf: the sea floor is flat.
    local height = Combine.Max{ a = settled, b = Terrain.Const{ value = floor } }

    return {
        height  = { value = height, range = { floor - 30, peak + 140 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = height } } },
    }
end

return main
