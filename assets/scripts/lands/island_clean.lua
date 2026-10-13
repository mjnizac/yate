-- Island, clean: one generated outline with no warp and no islets. The reference for the signed
-- distance, the mask and the margin; everything else is this plus a band or a coast type.
--
--   terrain_viewer --script assets/scripts/lands/island_clean.lua --water
--
-- Parameters: radius (metres), margin ("compressed" or "geologic"), height (metres).

local function main(params)
    local island = Lands.Island{
        seed = params.seed, resolution = params.resolution,
        radius = params.radius or 3000.0, roughness = 0.0, breakage = 0,
        height = params.height or 380.0, margin = params.margin or "compressed",
    }
    return island:outputs{}
end

return main
