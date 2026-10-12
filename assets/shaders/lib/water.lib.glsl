// Water shading shared by the terrain fragment stage and the open-sea quad.
//
// The water is never a plane drawn over the terrain: two surfaces at nearly the same depth z-fight
// wherever the terrain crosses the level, which is the whole shoreline and every flat area near it.
// Instead the terrain fragment computes the water in front of itself analytically, from the eye, the
// fragment and the level (see `terrain.frag.glsl`), and the sea quad only covers pixels no terrain
// reached. Nothing is rasterized twice, so there are no two depths to compare.

// What the surface mirrors. Pale and cool, brighter than the clear colour so the sea reads as a
// surface against the background rather than as a hole in it.
const vec3 kWaterSky = vec3(0.55, 0.66, 0.80);

// Schlick's approximation for a dielectric: about 2% reflected looking straight down, all of it at the
// horizon. `cosTheta` is the cosine of the angle between the view ray and the surface normal.
float WaterFresnel(float cosTheta) {
    float m = 1.0 - clamp(cosTheta, 0.0, 1.0);
    float m2 = m * m;
    return 0.02 + 0.98 * m2 * m2 * m;
}

// The colour of the water surface seen along `direction` (unit, from the eye to the surface).
vec3 WaterSurface(vec3 body, vec3 direction) {
    return mix(body, kWaterSky, WaterFresnel(abs(direction.y)));
}
