-- Coast: a landmass whose shore is a cliff along one side and a beach along the other, over a sea shelf.
--
-- The coastline is a closed spline filled to a signed distance `d` (negative inland, positive at sea),
-- warped so it is not a drawing of a curve. Everything else is a function of that one distance:
--
--   * the **beach** rises from just under the waterline over a hundred and fifty metres, then climbs
--     slowly inland;
--   * the **cliff** is a plateau profile of `d`, so the ground goes from the sea floor to its full height
--     over a few tens of metres;
--   * which one a stretch of shore gets is a feathered mask of an ellipse laid over the coast;
--   * beyond the shore `d` is positive and the ground falls away as a shelf.
--
-- It is meant to be viewed with water at 0 m, which is where the beach and the cliff foot are.
--
--   terrain_viewer --script assets/scripts/shapes/coast.lua --water --view 200,-26,3800
--
-- Parameters (metres): cliff is the cliff height, beach how far inland the beach runs, shelf how deep the
-- sea floor goes, warp how far the coastline wanders.

local function main(params)
    local cliff = params.cliff or 95.0
    local beach = params.beach or 170.0
    local shelf = params.shelf or 70.0
    local warp  = params.warp or 90.0

    local position = Shapes.Warp{ amplitude = warp, frequency = 0.0021, octaves = 3, seed = params.seed }

    -- The land: a bay on the east side and a headland on the west, as a closed curve.
    local land = Shapes.Spline{
        points = {
            { -2300, 900 }, { -1700, -250 }, { -1000, -700 }, { -250, -450 }, { 250, -900 },
            { 1000, -500 }, { 1500, -1100 }, { 2300, -800 }, { 2300, 1500 }, { 1000, 2300 },
            { -1200, 2300 }, { -2300, 1700 },
        },
        subdivisions = 6, closed = true, fill = true, position = position,
    }
    -- Seen as heights: `inland` grows with the distance from the shore, 0 at sea.
    local inland = Curves.Clamp{ input = land * -1.0, min = 0.0, max = 4000.0 }

    -- Beach: just under the water at the shore, a berm, then the ground rising away from it.
    local berm = Curves.Smoothstep{ input = land * -1.0, min = 0.0, max = beach }
    local rise = Curves.Smoothstep{ input = land * -1.0, min = beach, max = 1400.0 }
    local beach_height = -5.0 + berm * 11.0 + rise * 85.0

    -- Cliff: a wall a few tens of metres wide. `edge` is how far inland it reaches full height, and
    -- `falloff` how far out to sea its foot is.
    local wall = Curves.Profile{ input = land, kind = "plateau", edge = 40.0, falloff = 30.0 }
    local cliff_height = wall * cliff + Curves.Smoothstep{ input = land * -1.0, min = 40.0, max = 900.0 } * 40.0

    -- Where the cliff is: the west headland, with a long feather so the two types of shore grade into
    -- each other instead of meeting at a seam.
    local cliff_zone = Shapes.Mask{
        shape   = Shapes.Ellipse{ at = { -1250, -450 }, radii = { 1700, 950 }, rotation = 18,
                                  position = position },
        feather = 520.0,
    }

    local shore = Combine.Blend{ a = beach_height, b = cliff_height, mask = cliff_zone }

    -- The sea shelf: positive `d` falls away to `shelf` metres below the surface and no further.
    local out_to_sea = Curves.Smoothstep{ input = land, min = 0.0, max = 1100.0 }
    local floor      = -out_to_sea * shelf

    local ground = Combine.Max{ a = shore, b = floor }
    local at_sea = Curves.Smoothstep{ input = land, min = -30.0, max = 120.0 }
    local base   = ground * (1.0 - at_sea) + floor * at_sea

    -- Relief: broad swells inland, rock detail on the cliffs. Both are masked to the land.
    local swell = Noises.fBm{
        frequency = 0.0017, octaves = 5, persistence = 0.5, amplitude = 26.0, seed = params.seed + 1,
    }
    local rock = Noises.Ridged{
        frequency = 0.006, octaves = 4, amplitude = 18.0, seed = params.seed + 2,
    }
    local on_land   = Curves.Smoothstep{ input = inland, min = 25.0, max = 500.0 }
    local on_cliffs = Curves.Smoothstep{ input = inland, min = 0.0, max = 120.0 } * cliff_zone
    local relief    = swell.value * on_land + rock.value * on_cliffs

    local carved = Erosion.Hydraulic{
        input = base + relief, iterations = 14, rain = 0.3, capacity = 30.0,
    }
    local smoothed = Filters.Blur{ input = carved, radius = 1 }
    -- A high talus: scree gathers at the foot of the cliff without rounding its top off.
    local settled = Erosion.Thermal{
        input = smoothed, iterations = 10, talus = math.tan(math.rad(50.0)), strength = 0.3,
    }

    return {
        height  = { value = settled, range = { -shelf - 40, cliff + 160 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
    }
end

return main
