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

-- Smoothstep on [0, 1], clamped.
function engine.smooth(x)
    if x <= 0.0 then return 0.0 end
    if x >= 1.0 then return 1.0 end
    return x * x * (3.0 - 2.0 * x)
end
