-- Preview gallery for docs/lua_reference.md. One case per --param case=<name>.
--
-- Every case renders over the same 2048 m square at 4 m per sample, so the images are comparable.
-- Fields that are already normalized export over [-1, 1]; heights say their own range.

local SPACING = 4.0

local function base(o)
    o = o or {}
    return Noises.fBm{
        frequency   = o.frequency   or 0.0012,
        octaves     = o.octaves     or 6,
        lacunarity  = o.lacunarity  or 2.0,
        persistence = o.persistence or 0.5,
        amplitude   = o.amplitude   or 1.0,
        kind        = o.kind        or "simplex",
        seed        = o.seed        or 0,
    }
end

local function height(o)
    o = o or {}
    o.amplitude = o.amplitude or 300.0
    o.frequency = o.frequency or 0.0006
    return base(o)
end

local cases = {}

-- Noise kinds.
cases.noise_simplex = function() return { height = { value = base{ kind = "simplex" }.value, range = { -1, 1 } } } end
cases.noise_ridged  = function() return { height = { value = base{ kind = "ridged"  }.value, range = { -1, 1 } } } end
cases.noise_billow  = function() return { height = { value = base{ kind = "billow"  }.value, range = { -1, 1 } } } end

-- Octaves.
for _, n in ipairs{ 1, 3, 8 } do
    cases["noise_octaves_" .. n] = function()
        return { height = { value = base{ octaves = n }.value, range = { -1, 1 } } }
    end
end

-- Persistence.
for _, p in ipairs{ 30, 50, 80 } do
    cases["noise_persistence_" .. p] = function()
        return { height = { value = base{ persistence = p / 100.0 }.value, range = { -1, 1 } } }
    end
end

-- Lacunarity.
for _, l in ipairs{ 15, 20, 35 } do
    cases["noise_lacunarity_" .. l] = function()
        return { height = { value = base{ lacunarity = l / 10.0 }.value, range = { -1, 1 } } }
    end
end

-- Frequency.
for _, f in ipairs{ 4, 12, 40 } do
    cases["noise_frequency_" .. f] = function()
        return { height = { value = base{ frequency = f / 10000.0 }.value, range = { -1, 1 } } }
    end
end

-- Terrain.
cases.terrain_normals = function()
    local b = height()
    return { height = { value = Terrain.Normals{ input = b.gradient } } }
end
cases.terrain_gradient = function()
    local b = height()
    return { height = { value = Terrain.Gradient{ input = b.value } } }
end
cases.terrain_coords = function()
    return { height = { value = Terrain.Coords{ domain = 2 }.x, range = { 0, 512 } } }
end

-- Slope mask. The thresholds bracket the base's measured slope distribution: p10 0.27, p50 0.69,
-- p90 1.23 for the six-octave gradient.
local function shape(o) return base{ frequency = 0.0006, octaves = o, amplitude = 300.0 } end

cases.mask_slope_noisy = function()
    return { height = { value = Masks.Slope{ input = shape(6).gradient, min = 0.3, max = 1.1 },
                        range = { 0, 1 } } }
end
cases.mask_slope_shape = function()
    return { height = { value = Masks.Slope{ input = shape(3).gradient, min = 0.3, max = 1.1 },
                        range = { 0, 1 } } }
end
cases.mask_slope_tight = function()
    return { height = { value = Masks.Slope{ input = shape(3).gradient, min = 0.6, max = 0.75 },
                        range = { 0, 1 } } }
end

-- Combine.
cases.combine_blend = function()
    local b = height()
    local r = Noises.Ridged{ frequency = 0.0012, octaves = 4, amplitude = 180.0, seed = 1 }
    return { height = { value = Combine.Blend{ a = b.value, b = r.value,
                                               mask = Masks.Slope{ input = shape(3).gradient,
                                                                   min = 0.3, max = 1.1 } },
                        range = { -400, 400 } } }
end
cases.combine_min = function()
    local b = height()
    local r = Noises.Ridged{ frequency = 0.0012, octaves = 4, amplitude = 180.0, seed = 1 }
    return { height = { value = Combine.Min{ a = b.value, b = r.value }, range = { -400, 400 } } }
end
cases.combine_max = function()
    local b = height()
    local r = Noises.Ridged{ frequency = 0.0012, octaves = 4, amplitude = 180.0, seed = 1 }
    return { height = { value = Combine.Max{ a = b.value, b = r.value }, range = { -400, 400 } } }
end

-- Curves.
cases.curve_none = function() return { height = { value = base().value, range = { -1, 1 } } } end
cases.curve_clamp = function()
    return { height = { value = Curves.Clamp{ input = base().value, min = -0.3, max = 0.3 }, range = { -1, 1 } } }
end
cases.curve_remap = function()
    return { height = { value = Curves.Remap{ input = base().value, min = 0.0, max = 1.0 }, range = { -1, 1 } } }
end
cases.curve_power_half = function()
    return { height = { value = Curves.Power{ input = base().value, exponent = 0.5 }, range = { -1, 1 } } }
end
cases.curve_power_three = function()
    return { height = { value = Curves.Power{ input = base().value, exponent = 3.0 }, range = { -1, 1 } } }
end
cases.curve_smoothstep = function()
    return { height = { value = Curves.Smoothstep{ input = base().value, min = -0.2, max = 0.2 }, range = { -1, 1 } } }
end

-- Blur.
for _, r in ipairs{ 1, 8, 32 } do
    cases["blur_" .. r] = function()
        return { height = { value = Filters.Blur{ input = base().value, radius = r }, range = { -1, 1 } } }
    end
end

-- Erosion. talus is a drop per cell, so it is quoted here as spacing * tan(angle).
local function talus_for(degrees) return SPACING * math.tan(math.rad(degrees)) end

cases.erosion_none = function()
    return { height = { value = height().value, range = { -400, 400 } } }
end
for _, n in ipairs{ 8, 32, 96 } do
    cases["erosion_thermal_" .. n] = function()
        return { height = { value = Erosion.Thermal{ input = height().value, iterations = n,
                                                     talus = talus_for(35), strength = 0.3 },
                            range = { -400, 400 } } }
    end
end
for _, n in ipairs{ 8, 32 } do
    cases["erosion_hydraulic_" .. n] = function()
        return { height = { value = Erosion.Hydraulic{ input = height().value, iterations = n,
                                                       rain = 0.5, capacity = 6.0 },
                            range = { -400, 400 } } }
    end
end
cases.erosion_flow = function()
    local carved = Erosion.Hydraulic{ input = height().value, iterations = 32, rain = 0.5, capacity = 6.0 }
    return { height = { value = carved.flow } }
end
cases.erosion_combined = function()
    local carved = Erosion.Hydraulic{ input = height().value, iterations = 32, rain = 0.5, capacity = 6.0 }
    local settled = Erosion.Thermal{ input = carved, iterations = 16, talus = talus_for(35), strength = 0.3 }
    return { height = { value = settled, range = { -400, 400 } } }
end

local function main(params)
    local name = params.case
    local build = cases[name]
    if build == nil then
        error("unknown case '" .. tostring(name) .. "'")
    end
    return build()
end

return main
