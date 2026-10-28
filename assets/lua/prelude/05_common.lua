-- Common: plain-Lua helpers the prelude's macros share.
--
-- They go on the `engine` table every prelude file receives, not on a global, so a script cannot reach
-- or replace them. Integers and floats only, so every machine gets the same numbers.
--
-- Run by the engine before every other prelude file.

local engine = ...

-- The lattice hash of the value noises: (ix, iz, salt) to [0, 1], for one seed.
function engine.lattice_hash(seed)
    return function(ix, iz, salt)
        local v = (ix * 374761393 + iz * 668265263 + salt * 2246822519 + seed * 3266489917) & 0xffffffff
        v = ((v ~ (v >> 13)) * 1274126177) & 0xffffffff
        return ((v ~ (v >> 16)) & 0xffff) / 65535.0
    end
end

-- A hash of one integer and a salt to [0, 1], for one seed: what a layout draws its choices from.
function engine.hasher(seed)
    return function(i, salt)
        local v = (i * 374761393 + (salt or 0) * 668265263 + seed * 2246822519) & 0xffffffff
        v = ((v ~ (v >> 13)) * 1274126177) & 0xffffffff
        return ((v ~ (v >> 16)) & 0xffff) / 65535.0
    end
end

-- A value as JSON: tables with only 1..n keys are arrays, others objects with their keys sorted (so the
-- same report is the same text); a number that is not finite is null.
local function json(value)
    local kind = type(value)
    if kind == "number" then
        if value ~= value or value == math.huge or value == -math.huge then return "null" end
        if math.type(value) == "integer" then return tostring(value) end
        return string.format("%.9g", value)
    elseif kind == "string" then
        return '"' .. value:gsub('[%c"\\]', function(c)
            return ({ ['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\t'] = '\\t' })[c]
                   or string.format("\\u%04x", c:byte())
        end) .. '"'
    elseif kind == "boolean" then
        return tostring(value)
    elseif kind == "table" then
        local n = #value
        local keys = {}
        for k in pairs(value) do keys[#keys + 1] = k end
        if #keys == n then
            local items = {}
            for i = 1, n do items[i] = json(value[i]) end
            return "[" .. table.concat(items, ",") .. "]"
        end
        table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
        local items = {}
        for _, k in ipairs(keys) do items[#items + 1] = json(tostring(k)) .. ":" .. json(value[k]) end
        return "{" .. table.concat(items, ",") .. "}"
    end
    return "null"
end
engine.json = json

-- What the prelude says about what it built, written to an export's metadata as `script_report`:
-- `engine.report(key, value)` sets one entry of that object.
local reported = {}
function engine.report(key, value)
    reported[key] = value
    engine.set_report(json(reported))
end

-- Smoothstep on [0, 1], clamped.
function engine.smooth(x)
    if x <= 0.0 then return 0.0 end
    if x >= 1.0 then return 1.0 end
    return x * x * (3.0 - 2.0 * x)
end
