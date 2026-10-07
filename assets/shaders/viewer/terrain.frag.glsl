// Terrain shading for the preview.
//
// Deliberately plain: one directional light, a height and slope ramp, and a distance fade. The point of
// the viewer is to judge the *terrain*, so the shading exists to make shape readable and nothing else.
// Anything fancier would hide the thing being looked at.

#version 460

// No push constants here on purpose. Everything this stage needs is interpolated, and declaring the
// block again would mean keeping two copies of it in step; the pipeline layout exposes the range to the
// vertex stage only.

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

    outColor = vec4(lit, 1.0);
}
