-- Coastal plateau: an island plateau whose coast is a cliff of clean faces onto a flat shelf under the sea,
-- the cliff type of the user's Minecraft maps (1 m is one block, the sea is y 62 there and 0 here), built
-- with the techniques of `terrains/cliffs.lua` scaled to a wall a tenth as high.
--
--   tools/dev.sh sweep assets/scripts/terrains/coastal_plateau.lua --vary stage=1,2,3,4 --bounds -1016,-1016,1016,1016 --res 1 --level 0 --sun 315,40
--   terrain_viewer --script assets/scripts/terrains/coastal_plateau.lua --bounds -1016,-1016,1016,1016 --resolution 1 --water --target 600,20,0 --view 250,-18,500
--
-- Measured on the maps and kept here (all metres): the plateau top 38 over the sea (y 100); the cliff 50
-- high from the top to the shelf, about 65 degrees at its steepest and 50 on average, with a sharp foot; the
-- shelf exactly flat 12 under the sea (y 50), about 110 wide. The sea floor is the island macros' business:
-- here it is flat.
--
-- Built in stages, each reviewable with `--param stage=N`:
--   1. Forms, no noise: the outline from plain shapes; the wall a hard cap rock (the top third, 65 degrees)
--      over a softer base (45 degrees), as `cliffs.lua` builds its tiers; the treads lean 1 % to the sea.
--   2. Structure: spurs of the wall every 100 to 200 m; the wall broken into blocks along distorted F3 - F1
--      cells (planar faces, sharp breaks); strata ledges on the walls only.
--   3. Faces: the expander (an octagon over the cliff band only, wider on the headlands), which turns
--      every rounded outline into straight faces; then talus aprons at 35 degrees against the wall's foot.
--   4. Displacement: the outline read through a broad warp (applied to the forms: displacing the expanded
--      field needs a GPU displacement op the engine does not have yet).
--
-- Parameters: `seed`, `stage` (1 to 4), `top`, `shelf`, `floor` (metres, sea at 0), `shelf_width`,
-- `columns` (metres the blocks break out), `expand` (the expander's half-window, metres), `shape` (its window:
-- square, octagon, diamond or disk), `displace` (the displacement's amplitude, metres).

local function main(params)
    local seed       = params.seed or 0
    local stage      = params.stage or 4
    local top        = params.top or 38.0
    local shelf      = params.shelf or -12.0
    local floor      = params.floor or -30.0
    local shelfWidth = params.shelf_width or 110.0
    local columns    = params.columns or 6.0
    local expand     = params.expand or 20.0
    local displace   = params.displace or 40.0
    local rise       = top - shelf

    -- 4. Displacement, applied to the forms: the outline read through a broad warp. Displacing after the
    -- expander needs the expanded field resampled, and `Terrain.Sample` (the CPU path) passes neighbourhood
    -- ops through untouched, so the expander was silently lost; until there is a GPU displacement op the
    -- faces keep the expander's directions and the outline bends instead.
    local position = stage >= 4 and Shapes.Warp{ amplitude = displace, frequency = 1 / 700, octaves = 1, seed = seed }
                     or Terrain.Coords{}

    -- 1. Forms. The outline: lobes of plain ellipses (land d < 0).
    local d = Shapes.Union{
        Shapes.Ellipse{ radii = { 640, 500 }, rotation = 15, position = position },
        Shapes.Ellipse{ at = { -470, -430 }, radii = { 330, 200 }, rotation = -35, position = position },
        Shapes.Ellipse{ at = { 500, 400 }, radii = { 300, 180 }, rotation = 40, position = position },
        Shapes.Ellipse{ at = { -600, 250 }, radii = { 220, 140 }, rotation = 75, position = position },
        Shapes.Ellipse{ at = { 540, -400 }, radii = { 200, 130 }, rotation = -60, position = position },
        smooth = 120.0 }
    local onShelf = 1.0 - Curves.Smoothstep{ input = d - shelfWidth, min = -2.0, max = 2.0 }
    local sea = floor + (shelf - floor) * onShelf

    -- Differential erosion, as `cliffs.lua`: a hard cap rock stands steep on a softer base that weathers back.
    -- Each part is a straight ramp, so its faces stay planes. The base rises from the foot to `1 - share` of
    -- the wall, the cap from there to the top; the foot is placed so the coast (height 0) lies at e = 0.
    -- Returns the wall's height over the shelf and `u`, metres inland of the foot.
    local function wall(e, share)
        local capRise, baseRise = rise * share, rise * (1.0 - share)
        local baseRun = baseRise / math.tan(math.rad(45.0))
        local capRun  = capRise / math.tan(math.rad(65.0))
        local u = -shelf / math.tan(math.rad(45.0)) - e
        local base = Curves.Clamp{ input = u / baseRun, min = 0.0, max = 1.0 }
        local cap  = Curves.Clamp{ input = (u - baseRun) / capRun, min = 0.0, max = 1.0 }
        -- The treads lean 1 % towards the sea, as benches do (flat ones hold water).
        local lean = Curves.Clamp{ input = (u - baseRun - capRun) * 0.01, min = 0.0, max = 2.0 }
        return baseRise * base + capRise * cap + lean, u
    end

    local e, share, headland = d, 0.35, 1.0
    if stage >= 2 then
        -- Spurs every 100 to 200 m, pushed out up to 45 m (what the expander squares into blocks).
        local spurs = Noises.fBm{ frequency = 1 / 220, octaves = 2, amplitude = 45.0, offset = 20.0, seed = seed + 1 }
        -- Blocks: distorted F3 - F1 cells move the wall in straight steps with sharp breaks (World Machine's
        -- Mountain Cliffs: distorted Voronoi as break points), `columns` metres, every 40 m or so.
        local cracks = Noises.Cellular{ order = 3, frequency = 1 / 40, octaves = 1, amplitude = columns, seed = seed + 3,
                                        position = Shapes.Warp{ amplitude = 6.0, frequency = 1 / 60, octaves = 1, seed = seed + 4 } }
        e = d - spurs.value + cracks.value
        -- 1 on the headlands (the spurs pushed out furthest), 0 in the bays.
        headland = Curves.Smoothstep{ input = spurs.value, min = 0.0, max = 45.0 }
        -- The cap takes 25 to 45 % of the wall, varying along the coast.
        share = 0.35 + 0.2 * Noises.fBm{ frequency = 1 / 600, octaves = 1, amplitude = 0.5, seed = seed + 5 }.value
    end
    local form, u = wall(e, share)
    local h = sea + form

    if stage >= 2 then
        -- Strata along the slope, as `cliffs.lua`: the residue of a soft terrace of the height, times how steep
        -- the wall is, so the top moves nowhere and the walls get ledges. Beds of irregular thickness every 8 m
        -- (the height remapped by 1.5 m of noise along itself), stepping back 2.4 m.
        local steep = Masks.Slope{ input = Terrain.Gradient{ input = h }, min = 0.6, max = 1.5 }
        local wobble = Noises.fBm{ frequency = 1 / 15, octaves = 2, amplitude = 1.5, seed = seed + 6,
                                   position = Vec.Combine{ h, Terrain.Const{ value = 0.0 } } }
        local bed = (h + wobble.value) / 8.0
        h = h + (bed - Curves.Terrace{ input = bed, step = 1.0, sharpness = 0.5 }) * steep * 2.4
    end

    -- 3. Faces: the expander, its window in samples. An octagon by default, so the faces take eight
    -- directions rather than the square's two; its radius map keeps it to the cliff band (from 80 m out on
    -- the shelf to 80 m inland of the foot, past the top edge) and gives the headlands the whole window and
    -- the bays 40 % of it.
    if stage >= 3 then
        local spacing = params.resolution
        local band = Curves.Smoothstep{ input = u, min = -80.0, max = -50.0 }
                   * (1.0 - Curves.Smoothstep{ input = u, min = 70.0, max = 100.0 })
        h = Filters.Expand{ input = h, radius = math.min(32, math.max(1, math.floor(expand / spacing + 0.5))),
                            shape = params.shape or "octagon", radius_map = band * (0.4 + 0.6 * headland) }
        -- Talus aprons, as `cliffs.lua`: a wedge at 35 degrees leaning on the wall's foot, reaching 6 to 20 m
        -- out, capped at a third of the wall; the larger of it and the ground buries the lower base in scree.
        local spread = Noises.fBm{ frequency = 1 / 180, octaves = 2, amplitude = 7.0, offset = 13.0, seed = seed + 7 }
        local wedge = Curves.Clamp{ input = (spread.value + u) * math.tan(math.rad(35.0)), min = 0.0, max = rise * 0.35 }
        h = Combine.Max{ a = h, b = sea + wedge }
    end

    return {
        height  = { value = h, range = { -50, 80 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = h } } },
    }
end

return main
