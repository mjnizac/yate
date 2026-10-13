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

// About 32 degrees up and warm: low enough that a face turned from it is in shade and ridges and gullies
// read as light against dark, high enough that a valley floor is still lit. The sky fills the shade.
const vec3 kSunDirection = normalize(vec3(0.55, 0.42, 0.40));
const vec3 kSunColor     = vec3(1.05, 0.98, 0.88);
const vec3 kSkyColor     = vec3(0.34, 0.42, 0.56);

// Grass where it is gentle and low, scree above it, rock where it is steep and snow where it is high and
// not too steep to hold it. Heights are in metres and fixed, so a low terrain is never snowy and the
// colour of a point does not depend on what else the script made.
const vec3 kGrassColor = vec3(0.22, 0.32, 0.16);
const vec3 kScreeColor = vec3(0.47, 0.45, 0.42);
const vec3 kRockColor  = vec3(0.30, 0.29, 0.28);
const vec3 kSnowColor  = vec3(0.90, 0.92, 0.96);
const float kScreeLine = 1250.0;
const float kSnowLine  = 1500.0;

// Rock texture, which the heightfield cannot carry: it is sampled every few metres at best and every 16
// to 32 in a distant view. Where the ground is steep, a ridged noise in world space, stretched about
// eight times along the vertical, bumps the normal and shades the albedo. On a steep face the vertical
// lies nearly in the surface, so the stretch turns into ribs and runnels running down the face, as rock
// weathers. Two octaves, each faded out once a pixel covers it so it never aliases; flat ground gets
// none and stays one colour. (Voronoi blocks were tried: from a kilometre away they read as paving.)
float Hash31(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float ValueNoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = p - i;
    f      = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(Hash31(i), Hash31(i + vec3(1, 0, 0)), f.x),
                   mix(Hash31(i + vec3(0, 1, 0)), Hash31(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(Hash31(i + vec3(0, 0, 1)), Hash31(i + vec3(1, 0, 1)), f.x),
                   mix(Hash31(i + vec3(0, 1, 1)), Hash31(i + vec3(1, 1, 1)), f.x), f.y),
               f.z);
}

float Ribs(vec3 p, float footprint) {
    float sum        = 0.0;
    float wavelength = 40.0;
    float amplitude  = 1.0;
    for (int o = 0; o < 2; ++o) {
        float fade = 1.0 - smoothstep(wavelength * 0.3, wavelength * 0.7, footprint);
        float n    = ValueNoise(p * vec3(1.0, 0.12, 1.0) / wavelength) * 2.0 - 1.0;
        sum += (1.0 - 2.0 * abs(n)) * amplitude * fade;
        wavelength *= 0.4;
        amplitude *= 0.5;
    }
    return sum;
}

void main() {
    vec3 normal = normalize(vNormal);
    // A normal buffer that was never written, or a degenerate one, would come through as zero and make
    // everything black. Facing up is the honest fallback: it says "flat", not "broken".
    if (dot(normal, normal) < 1e-6) {
        normal = vec3(0.0, 1.0, 0.0);
    }

    float slope  = 1.0 - clamp(normal.y, 0.0, 1.0);
    float height = vWorldPosition.y;
    float rocky  = smoothstep(0.16, 0.42, slope);
    // Snow lies where the ground is smooth enough to hold it and high enough to keep it; the rock
    // shows through where it is steep. The line wavers with the rock noise so it is not a contour.
    float footprint = max(length(fwidth(vWorldPosition)), 1e-3);
    float rock      = rocky > 0.0 ? Ribs(vWorldPosition, footprint) : 0.0;
    // Snow lies where the ground is smooth enough to hold it and high enough to keep it; the rock
    // shows through where it is steep. The line wavers with the rock so it is not a contour.
    float snow = smoothstep(kSnowLine - 150.0, kSnowLine + 150.0, height + rock * 80.0)
               * (1.0 - smoothstep(0.34, 0.5, slope + rock * 0.08));

    vec3 albedo = mix(kGrassColor, kScreeColor, smoothstep(kScreeLine - 300.0, kScreeLine + 200.0, height));
    // Rock: darker in the runnels, lighter on the ribs.
    vec3 rockColor = kRockColor * (0.82 + 0.28 * rock);
    albedo         = mix(albedo, rockColor, rocky);
    albedo         = mix(albedo, kSnowColor, snow);

    // Bump the normal with the ribs' gradient (central differences in world space), on rock only, so
    // snow stays smooth.
    float weight = rocky * (1.0 - snow);
    if (weight > 0.0) {
        const float e = 1.0;
        vec3 g = vec3(Ribs(vWorldPosition + vec3(e, 0, 0), footprint)
                          - Ribs(vWorldPosition - vec3(e, 0, 0), footprint),
                      0.0,
                      Ribs(vWorldPosition + vec3(0, 0, e), footprint)
                          - Ribs(vWorldPosition - vec3(0, 0, e), footprint)) / (2.0 * e);
        g -= dot(g, normal) * normal;
        normal = normalize(normal - g * (7.0 * weight));
    }

    float sun = max(dot(normal, kSunDirection), 0.0);
    // Hemispheric ambient rather than a constant: it brightens upward faces and darkens downward ones,
    // which is most of what makes an unlit slope legible.
    float sky = 0.5 + 0.5 * normal.y;

    vec3 lit = albedo * (kSunColor * sun + kSkyColor * sky * 0.4);

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
