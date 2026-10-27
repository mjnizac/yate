-- Drawn: a terrain built from shapes drawn elsewhere and handed over as GeoJSON with `--shapes`.
--
-- The script reads `params.shapes`, one table per feature with its `kind`, its other properties and its
-- `points` in world metres, and turns each kind into a landform:
--
--   * `land` (Polygon): the coast; the ground rises inland of it and falls to a shelf beyond;
--   * `ridge` (LineString, `height`, `width`): a range along the line;
--   * `lake` (Polygon, `level`, `depth`): water at `level` metres in a basin `depth` deep under it;
--   * `river` (LineString, `width`, `depth`): a channel cut along the line.
--
--   terrain_viewer --script assets/scripts/shapes/drawn.lua --shapes assets/shapes/valley.geojson --water
--
-- Any GeoJSON editor draws these (geojson.io, QGIS): the coordinates are world x and z in metres, and the
-- file is reloaded when it changes, as the script is.

local KINDS = { land = "polygon", ridge = "line", lake = "polygon", river = "line" }

local function main(params)
    if params.shapes == nil then
        error("drawn.lua needs its shapes: run it with --shapes <file.geojson>")
    end
    local position = Shapes.Warp{ amplitude = 60.0, frequency = 0.002, octaves = 3, seed = params.seed }

    local lands, ridges, lakes, rivers = {}, {}, {}, {}
    local lists = { land = lands, ridge = ridges, lake = lakes, river = rivers }
    for i, shape in ipairs(params.shapes) do
        if KINDS[shape.kind] == nil or KINDS[shape.kind] ~= shape.geometry then
            error("shape " .. i .. ": a '" .. tostring(shape.kind) .. "' " .. shape.geometry
                .. " is not one of land, lake (polygons), ridge, river (lines)")
        end
        table.insert(lists[shape.kind], shape)
    end
    if #lands == 0 then
        error("drawn.lua needs at least one 'land' polygon")
    end

    -- The coast: the land polygons' union as one signed distance, negative inland.
    local coast = nil
    for _, land in ipairs(lands) do
        local d = Shapes.Spline{ points = land.points, closed = true, fill = true, subdivisions = 4,
                                 position = position }
        coast = coast == nil and d or Combine.Min{ a = coast, b = d }
    end
    local inland = coast * -1.0
    local ground = Curves.Smoothstep{ input = inland, min = -60.0, max = 2500.0 } * 160.0
        - Curves.Smoothstep{ input = coast, min = 0.0, max = 1200.0 } * 60.0 - 4.0

    for _, ridge in ipairs(ridges) do
        ground = ground + Landforms.Ridge{ points = ridge.points, width = ridge.width or 1500.0,
                                           height = ridge.height or 500.0, position = position }
    end

    -- Hills and peaks, stronger where the ridges are high, so the ranges are not smooth bands.
    local relief = Noises.Ridged{ frequency = 0.0011, octaves = 6, amplitude = 140.0, seed = params.seed + 1 }
    local on_land = Curves.Smoothstep{ input = inland, min = 0.0, max = 600.0 }
    ground = ground + relief.value * on_land * Curves.Smoothstep{ input = ground, min = 20.0, max = 500.0 }

    -- Lakes and rivers last, cut into whatever is there: a channel is never filled by a ridge it crosses.
    -- Each gives a water surface, far under the ground away from it, as the viewer's water reads one.
    local lake_water, river_water = nil, nil
    for _, lake in ipairs(lakes) do
        if type(lake.level) ~= "number" then
            error("a lake needs 'level', the height of its water in metres")
        end
        local d    = Shapes.Spline{ points = lake.points, closed = true, fill = true, subdivisions = 4,
                                    position = position }
        -- The floor is `depth` under the water, the shore 4 m over it 300 m out, and beyond that the basin
        -- rises at 45 degrees, so it only cuts what stands lower than that.
        local depth = lake.depth or 20.0
        local bowl  = Curves.Smoothstep{ input = d, min = -400.0, max = 300.0 } * (depth + 4.0)
            + Curves.Clamp{ input = d - 300.0, min = 0.0, max = 100000.0 }
        ground = Combine.Min{ a = ground, b = lake.level - depth + bowl }
        local surface = lake.level - Curves.Smoothstep{ input = d, min = 0.0, max = 40.0 } * 300.0
        lake_water = lake_water == nil and surface or Combine.Max{ a = lake_water, b = surface }
    end
    for _, river in ipairs(rivers) do
        local line  = Shapes.Spline{ points = river.points, subdivisions = 6, position = position }
        local half  = (river.width or 50.0) * 0.5
        local depth = river.depth or 5.0
        -- The channel, and a valley twenty widths wide that takes a quarter of the ground above 10 m.
        local cut    = Curves.Profile{ input = line - half, kind = "smooth", width = half * 4.0 }
        local valley = Curves.Profile{ input = line - half, kind = "smooth", width = half * 20.0 }
        local floor  = ground - valley * 0.25 * Curves.Clamp{ input = ground - 10.0, min = 0.0, max = 5000.0 }
        ground = floor - cut * depth * on_land
        local surface = floor - depth * 0.4 - Curves.Smoothstep{ input = line - half, min = 0.0, max = 6.0 } * 300.0
        river_water = river_water == nil and surface or Combine.Max{ a = river_water, b = surface }
    end

    local carved  = Erosion.Hydraulic{ input = ground, iterations = 12, rain = 0.3, capacity = 20.0 }
    local settled = Erosion.Thermal{ input = Filters.Blur{ input = carved, radius = 1 }, iterations = 8,
                                     talus = math.tan(math.rad(38.0)), strength = 0.3 }
    local out = {
        height  = { value = settled, range = { -120, 1200 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
    }
    if lake_water ~= nil then
        out.lakes = { value = lake_water, range = { -400, 1200 } }
    end
    if river_water ~= nil then
        out.rivers = { value = river_water, range = { -400, 1200 } }
    end
    return out
end

return main
