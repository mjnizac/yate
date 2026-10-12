// Terrain shading for the preview.
//
// Deliberately plain: one directional light, a height and slope ramp, and a distance fade. The point of
// the viewer is to judge the *terrain*, so the shading exists to make shape readable and nothing else.
// Anything fancier would hide the thing being looked at.

#version 460

#extension GL_EXT_scalar_block_layout : require

#include "lib/water.lib.glsl"

// The same block the vertex stage declares, member for member under scalar layout. The two 64-bit
// addresses are `uvec2` here because this stage never dereferences them.
layout(push_constant, scalar) uniform Push {
    mat4  viewProjection;
    vec3  tileOrigin;
    float resolution;
    uint  samples;
    uint  gridVertices;
    float waterLevel;
    uint  waterColour;
    uvec2 heights;
    uvec2 normals;
    vec3  eye;
    uint  pad;
}
pc;

// Metres of water over which the terrain fades into the water colour. Short enough that a few metres of
// depth already reads as water, long enough that a deep basin does not go flat.
const float kExtinction = 30.0;
// Half-width of the shoreline band in metres, and its colour.
const float kShoreWidth = 1.5;
const vec3  kShoreColor = vec3(0.86, 0.88, 0.84);

layout(location = 0) in vec3 vWorldPosition;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in float vHeight;

layout(location = 0) out vec4 outColor;

// Low and warm, so slopes facing away are shaded rather than black and the relief reads at a glance.
const vec3 kSunDirection = normalize(vec3(0.45, 0.72, 0.52));
const vec3 kSunColor     = vec3(1.0, 0.96, 0.88);
const vec3 kSkyColor     = vec3(0.32, 0.40, 0.52);

// Rock where it is steep, grass where it is not. The band is wide so the transition is not a hard line.
const vec3 kRockColor  = vec3(0.42, 0.40, 0.38);
const vec3 kGrassColor = vec3(0.26, 0.36, 0.20);
const vec3 kPeakColor  = vec3(0.78, 0.78, 0.80);

void main() {
    vec3 normal = normalize(vNormal);
    // A normal buffer that was never written, or a degenerate one, would come through as zero and make
    // everything black. Facing up is the honest fallback: it says "flat", not "broken".
    if (dot(normal, normal) < 1e-6) {
        normal = vec3(0.0, 1.0, 0.0);
    }

    float slope = 1.0 - clamp(normal.y, 0.0, 1.0);
    vec3  albedo = mix(kGrassColor, kRockColor, smoothstep(0.15, 0.55, slope));
    // Lightest at the top, which reads as snow without needing a height range from the script.
    albedo = mix(albedo, kPeakColor, smoothstep(0.55, 0.8, normal.y) * smoothstep(0.3, 0.9, slope));

    float sun = max(dot(normal, kSunDirection), 0.0);
    // Hemispheric ambient rather than a constant: it brightens upward faces and darkens downward ones,
    // which is most of what makes an unlit slope legible.
    float sky = 0.5 + 0.5 * normal.y;

    vec3 lit = albedo * (kSunColor * sun + kSkyColor * sky * 0.35);

    // Water, computed from the eye and this fragment rather than drawn: alpha zero means none.
    vec4 water = unpackUnorm4x8(pc.waterColour);
    if (water.a > 0.0) {
        vec3  toFragment = vWorldPosition - pc.eye;
        float distance_  = length(toFragment);
        vec3  direction  = toFragment / max(distance_, 1e-4);

        // Where along eye -> fragment, as a fraction, the ray is below the level. `y(t)` is linear in t,
        // so the submerged part is one interval and one expression covers camera and fragment each on
        // either side of the surface: it begins at 0 if the eye is under and at the crossing if not, and
        // ends at 1 if the fragment is under and at the crossing if not.
        float dy       = toFragment.y;
        float crossing = abs(dy) > 1e-6 ? clamp((pc.waterLevel - pc.eye.y) / dy, 0.0, 1.0)
                                        : (pc.eye.y < pc.waterLevel ? 1.0 : 0.0);
        float begin    = pc.eye.y < pc.waterLevel ? 0.0 : crossing;
        float end      = vWorldPosition.y < pc.waterLevel ? 1.0 : crossing;
        float path     = max(end - begin, 0.0) * distance_;

        float cover = water.a * (1.0 - exp(-path / kExtinction));
        lit         = mix(lit, water.rgb, cover);

        // The surface is in front of this fragment only when the eye and the fragment are on opposite
        // sides of it. Then what the pixel shows is the surface, and a surface reflects.
        if ((pc.eye.y - pc.waterLevel) * (vWorldPosition.y - pc.waterLevel) < 0.0) {
            lit = mix(lit, WaterSurface(lit, direction), water.a);
        }

        // A pale band where the terrain meets the level. Its width follows the pixel footprint, so
        // it stays a line at a distance instead of shimmering as it shrinks below a pixel.
        float band = max(kShoreWidth, fwidth(vWorldPosition.y) * 1.5);
        lit        = mix(lit, kShoreColor, water.a * 0.6
                                               * (1.0 - smoothstep(0.0, band,
                                                                   abs(vWorldPosition.y
                                                                       - pc.waterLevel))));
    }

    outColor = vec4(lit, 1.0);
}
