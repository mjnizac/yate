-- Coastal plateau: an island plateau whose coast is a cliff onto a flat shelf under the sea, the cliff type
-- measured on the user's Minecraft maps (1 m is one block, the sea is y 62 there and 0 here).
--
--   tools/dev.sh review assets/scripts/terrains/coastal_plateau.lua --bounds -1016,-1016,1016,1016 --res 1 --level 0 --sun 315,40 --target 0,20,0 --shot 225,-30,1400 --shot 300,-12,700 --shot 0,-89,1800
--   terrain_viewer --script assets/scripts/terrains/coastal_plateau.lua --bounds -1016,-1016,1016,1016 --resolution 1 --water --target 0,20,0 --view 225,-30,1400
--
-- Measured on the maps and kept here (all metres):
--   * the plateau top 38 over the sea (y 100), the highest tops capped flat 2.4 m over the rest;
--   * the coastal cliff 50 high from the top to the shelf, about 65 degrees at its steepest, a rounded lip
--     and a sharp foot where it meets the shelf;
--   * the shelf exactly flat 12 under the sea (y 50), 80 to 200 wide, its outer edge a near-vertical riser
--     of 10 to 15 down to an uneven sea floor (y 20 to 38);
--   * inland cliffs follow the outlines of raised plateaus (not fixed heights): S-shaped risers 12 to 17
--     high at up to 65 degrees.
--
-- Built in stages, each reviewable with `--param stage=N`:
--   1. Forms: the outline, the coastal wall, the shelf and its riser, a flat sea floor, a flat top.
--   2. Structure: raised inner plateaus with their own risers, the cap, offshore islets on the shelf.
--   3. Faces: buttresses and chutes on the coastal wall, a sideways displacement of the outline near it.
--   4. Detail: gentle relief on the top, relief on the sea floor, half a metre of grain.
--
-- Parameters: `seed`, `stage` (1 to 4), `top`, `shelf`, `floor` (metres, sea at 0), `cliff` (the wall's
-- horizontal width), `shelf_width`.

local function main(params)
    local seed       = params.seed or 0
    local stage      = params.stage or 4
    local top        = params.top or 38.0
    local shelf      = params.shelf or -12.0
    local floor      = params.floor or -30.0
    local cliff      = params.cliff or 60.0
    local shelfWidth = params.shelf_width or 140.0
    local c          = Terrain.Coords{}

    -- The outline: five lobes bent by a broad warp, then lobes and bays of 400 m and a fray of 160 m along the
    -- coast (land d < 0). Their slopes stay under 1, so the wall keeps its width.
    local warp   = Shapes.Warp{ amplitude = 150.0, frequency = 0.0012, octaves = 3, seed = seed }
    local island = Shapes.Union{
        Shapes.Ellipse{ radii = { 700, 560 }, rotation = 15, position = warp },
        Shapes.Ellipse{ at = { -480, -460 }, radii = { 360, 220 }, rotation = -35, position = warp },
        Shapes.Ellipse{ at = { 520, 430 }, radii = { 330, 200 }, rotation = 40, position = warp },
        Shapes.Ellipse{ at = { -620, 260 }, radii = { 240, 150 }, rotation = 75, position = warp },
        Shapes.Ellipse{ at = { 560, -420 }, radii = { 220, 140 }, rotation = -60, position = warp },
        smooth = 120.0 }
    local lobes = Noises.fBm{ frequency = 1 / 400, octaves = 2, amplitude = 45.0, seed = seed + 7 }
    local fray = Noises.fBm{ frequency = 1 / 160, octaves = 4, amplitude = 12.0, seed = seed + 1 }
    local d = island + lobes.value + fray.value

    if stage >= 2 then
        -- Islets standing on the shelf, joined to the coast where they touch it.
        d = Combine.SmoothMin{ a = d, k = 20.0, b = Shapes.Union{
            Shapes.Circle{ at = { 690, -140 }, radius = 38, position = warp },
            Shapes.Circle{ at = { -150, 640 }, radius = 28, position = warp },
            Shapes.Ellipse{ at = { -700, 120 }, radii = { 55, 30 }, rotation = 60, position = warp } } }
    end
    if stage >= 3 then
        -- Buttresses and chutes: the outline pushed in and out every 40 m or so, only near the coast.
        local near = Curves.Smoothstep{ input = d, min = -120.0, max = -40.0 }
                   * (1.0 - Curves.Smoothstep{ input = d, min = 40.0, max = 120.0 })
        local flutes = Noises.Ridged{ frequency = 1 / 45, octaves = 2, amplitude = 4.0, seed = seed + 2 }
        d = d + flutes.value * near
    end

    -- The shelf: the coast pushed out by a varying width, flat at `shelf`, its edge a 4 m riser.
    local width = Noises.fBm{ frequency = 1 / 420, octaves = 2, amplitude = 60.0, offset = shelfWidth, seed = seed + 3 }
    local onShelf = 1.0 - Curves.Smoothstep{ input = d - width.value, min = -2.0, max = 2.0 }
    local seabed = Terrain.Const{ value = floor }
    if stage >= 4 then
        -- Mounds 50 to 150 m across, 10 m high, over 3 m of finer relief.
        seabed = seabed + Noises.Billow{ frequency = 1 / 200, octaves = 3, amplitude = 10.0, seed = seed + 4 }.value
               + Noises.fBm{ frequency = 1 / 60, octaves = 3, amplitude = 3.0, seed = seed + 8 }.value
    end
    local sea = seabed + (shelf - seabed) * onShelf

    -- The coastal wall: a smoothstep rise from the shelf to the top, so the coast (height 0) lies at d = 0.
    local rise = Curves.Smoothstep{ input = -d, min = -0.35 * cliff, max = 0.65 * cliff }
    local h = sea + (top - shelf) * rise

    if stage >= 2 then
        -- Raised inner plateaus, 16 m over the top, each with its own S-shaped riser 11 m wide.
        local upper = Shapes.Union{
            Shapes.Ellipse{ at = { -60, -90 }, radii = { 330, 210 }, rotation = 25, position = warp },
            Shapes.Ellipse{ at = { 300, 220 }, radii = { 170, 110 }, rotation = 50, position = warp },
            smooth = 80.0 } + fray.value * 0.6
            + Noises.fBm{ frequency = 1 / 300, octaves = 2, amplitude = 60.0, seed = seed + 9 }.value
        h = h + 16.0 * Curves.Smoothstep{ input = -upper, min = -6.0, max = 12.0 } * rise
    end
    if stage >= 4 then
        -- Relief on the top only, held off the wall by the rise: 4 m of broad swells over a dome 6 m higher
        -- inland, so most of the top drains to the coast; half a metre of grain on the wall only, since on the
        -- flat top it holds water in thousands of small pits.
        local dome = Curves.Smoothstep{ input = -d, min = 0.0, max = 600.0 }
        h = h + (Noises.fBm{ frequency = 1 / 500, octaves = 3, persistence = 0.4, amplitude = 4.0, seed = seed + 5 }.value
                 + 6.0 * dome) * rise
            + Noises.fBm{ frequency = 1 / 12, octaves = 3, amplitude = 0.5, seed = seed + 6 }.value * rise * (1.0 - rise) * 4.0
    end
    if stage >= 2 then
        -- The cap: the highest tops cut flat 2.4 m over the upper plateaus and their dome, the edge of the cut
        -- rounded over 2 m so it is not a crease.
        h = Combine.SmoothMin{ a = h, b = Terrain.Const{ value = top + 6.0 + 16.0 + 2.4 }, k = 2.0 }
    end

    return {
        height  = { value = h, range = { -50, 80 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = h } } },
    }
end

return main
