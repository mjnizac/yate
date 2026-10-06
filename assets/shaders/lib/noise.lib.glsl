// Simplex noise with analytic derivatives, and fractal accumulation on top of it.
//
// Every function here returns the value *and* its exact gradient, computed by differentiating the
// kernel in closed form rather than by finite differences. A terrain graph needs slopes, normals
// and curvature constantly; getting them analytically means one evaluation instead of three or
// more, and the result is exact rather than an approximation at some epsilon.
//
// These functions take no buffers and touch no push constants, so a future kernel-fusion pass can
// chain them directly (spec section 9, "Future: kernel fusion").

#ifndef ENGINE_LIB_NOISE_GLSL
#define ENGINE_LIB_NOISE_GLSL

#include "lib/hash.lib.glsl"

// --- Gradient sets ------------------------------------------------------------------------------

// Eight unit directions: the four axes and the four diagonals.
vec2 LatticeGradient2(ivec2 cell, uint seed) {
    uint h = HashLattice2(cell, seed) & 7u;
    const float k = 0.70710678118654752; // 1 / sqrt(2)
    switch (h) {
        case 0u: return vec2(1.0, 0.0);
        case 1u: return vec2(-1.0, 0.0);
        case 2u: return vec2(0.0, 1.0);
        case 3u: return vec2(0.0, -1.0);
        case 4u: return vec2(k, k);
        case 5u: return vec2(-k, k);
        case 6u: return vec2(k, -k);
        default: return vec2(-k, -k);
    }
}

// The twelve edge midpoints of a cube, the classic Perlin set. Length sqrt(2), which is what the
// 3D normalization constant below assumes.
vec3 LatticeGradient3(ivec3 cell, uint seed) {
    uint h = HashLattice3(cell, seed) % 12u;
    switch (h) {
        case 0u: return vec3(1.0, 1.0, 0.0);
        case 1u: return vec3(-1.0, 1.0, 0.0);
        case 2u: return vec3(1.0, -1.0, 0.0);
        case 3u: return vec3(-1.0, -1.0, 0.0);
        case 4u: return vec3(1.0, 0.0, 1.0);
        case 5u: return vec3(-1.0, 0.0, 1.0);
        case 6u: return vec3(1.0, 0.0, -1.0);
        case 7u: return vec3(-1.0, 0.0, -1.0);
        case 8u: return vec3(0.0, 1.0, 1.0);
        case 9u: return vec3(0.0, -1.0, 1.0);
        case 10u: return vec3(0.0, 1.0, -1.0);
        default: return vec3(0.0, -1.0, -1.0);
    }
}

// --- 2D simplex ---------------------------------------------------------------------------------

// Contribution of one simplex corner, together with its derivative.
//
//   n(d)  = w^4 * (g . d),      w = r2 - |d|^2
//   dn/dd = -8 w^3 (g . d) d + w^4 g
//
// `d` is the offset from the corner in unskewed space, and the cell origin is piecewise constant,
// so dd/dp is the identity and the expression above is the derivative with respect to the input.
vec3 SimplexCorner2(vec2 d, vec2 g, float r2) {
    float w = r2 - dot(d, d);
    if (w <= 0.0) {
        return vec3(0.0);
    }
    float w2 = w * w;
    float w3 = w2 * w;
    float w4 = w2 * w2;
    float gd = dot(g, d);
    return vec3(w4 * gd, (-8.0 * w3 * gd) * d + w4 * g);
}

// Chosen so the output stays within roughly [-1, 1] for the unit gradient set above. The exact
// bound is not critical: `terrain_export` reports how many samples the declared output range
// clamped, which is the signal that a scale or a range is wrong.
#define ENGINE_SIMPLEX2_SCALE 99.0

// Returns vec3(value, d/dx, d/dy) at `p`, in the units of `p`.
vec3 Simplex2D(vec2 p, uint seed) {
    const float F2 = 0.36602540378443865; // (sqrt(3) - 1) / 2
    const float G2 = 0.21132486540518712; // (3 - sqrt(3)) / 6

    // Skew into the simplex lattice to find the containing cell.
    vec2  skewed = p + (p.x + p.y) * F2;
    vec2  cell   = floor(skewed);
    vec2  origin = cell - (cell.x + cell.y) * G2;
    vec2  d0     = p - origin;

    // Which of the two triangles of the cell the point falls in.
    vec2 step1 = d0.x > d0.y ? vec2(1.0, 0.0) : vec2(0.0, 1.0);

    vec2 d1 = d0 - step1 + G2;
    vec2 d2 = d0 - 1.0 + 2.0 * G2;

    ivec2 i0 = ivec2(cell);
    vec3  sum = SimplexCorner2(d0, LatticeGradient2(i0, seed), 0.5);
    sum += SimplexCorner2(d1, LatticeGradient2(i0 + ivec2(step1), seed), 0.5);
    sum += SimplexCorner2(d2, LatticeGradient2(i0 + ivec2(1), seed), 0.5);
    return sum * ENGINE_SIMPLEX2_SCALE;
}

// --- 3D simplex ---------------------------------------------------------------------------------

vec4 SimplexCorner3(vec3 d, vec3 g, float r2) {
    float w = r2 - dot(d, d);
    if (w <= 0.0) {
        return vec4(0.0);
    }
    float w2 = w * w;
    float w3 = w2 * w;
    float w4 = w2 * w2;
    float gd = dot(g, d);
    return vec4(w4 * gd, (-8.0 * w3 * gd) * d + w4 * g);
}

#define ENGINE_SIMPLEX3_SCALE 32.0

// Returns vec4(value, d/dx, d/dy, d/dz) at `p`, in the units of `p`.
vec4 Simplex3D(vec3 p, uint seed) {
    const float F3 = 0.33333333333333333;
    const float G3 = 0.16666666666666666;

    vec3 cell   = floor(p + (p.x + p.y + p.z) * F3);
    vec3 origin = cell - (cell.x + cell.y + cell.z) * G3;
    vec3 d0     = p - origin;

    // Rank the components to pick the simplex: the two intermediate corners follow the descending
    // order of d0.
    vec3 step1;
    vec3 step2;
    if (d0.x >= d0.y) {
        if (d0.y >= d0.z) {
            step1 = vec3(1.0, 0.0, 0.0);
            step2 = vec3(1.0, 1.0, 0.0);
        } else if (d0.x >= d0.z) {
            step1 = vec3(1.0, 0.0, 0.0);
            step2 = vec3(1.0, 0.0, 1.0);
        } else {
            step1 = vec3(0.0, 0.0, 1.0);
            step2 = vec3(1.0, 0.0, 1.0);
        }
    } else {
        if (d0.y < d0.z) {
            step1 = vec3(0.0, 0.0, 1.0);
            step2 = vec3(0.0, 1.0, 1.0);
        } else if (d0.x < d0.z) {
            step1 = vec3(0.0, 1.0, 0.0);
            step2 = vec3(0.0, 1.0, 1.0);
        } else {
            step1 = vec3(0.0, 1.0, 0.0);
            step2 = vec3(1.0, 1.0, 0.0);
        }
    }

    vec3 d1 = d0 - step1 + G3;
    vec3 d2 = d0 - step2 + 2.0 * G3;
    vec3 d3 = d0 - 1.0 + 3.0 * G3;

    ivec3 i0  = ivec3(cell);
    vec4  sum = SimplexCorner3(d0, LatticeGradient3(i0, seed), 0.6);
    sum += SimplexCorner3(d1, LatticeGradient3(i0 + ivec3(step1), seed), 0.6);
    sum += SimplexCorner3(d2, LatticeGradient3(i0 + ivec3(step2), seed), 0.6);
    sum += SimplexCorner3(d3, LatticeGradient3(i0 + ivec3(1), seed), 0.6);
    return sum * ENGINE_SIMPLEX3_SCALE;
}

// --- Noise kinds --------------------------------------------------------------------------------

#define ENGINE_NOISE_SIMPLEX 0u
#define ENGINE_NOISE_RIDGED  1u
#define ENGINE_NOISE_BILLOW  2u

// Shapes one octave, carrying its derivative through the transform:
//
//   ridged: v = (1 - |n|)^2,  dv = -2 (1 - |n|) sign(n) dn
//   billow: v = 2|n| - 1,     dv = 2 sign(n) dn
//
// Both have a kink where n is exactly zero. That is inherent to the shape rather than a defect of
// the derivative, and the set of samples landing there has measure zero.
vec3 ShapeOctave2(uint kind, vec3 n) {
    if (kind == ENGINE_NOISE_RIDGED) {
        float s = n.x < 0.0 ? -1.0 : 1.0;
        float r = 1.0 - s * n.x;
        return vec3(r * r, (-2.0 * r * s) * n.yz);
    }
    if (kind == ENGINE_NOISE_BILLOW) {
        float s = n.x < 0.0 ? -1.0 : 1.0;
        return vec3(2.0 * s * n.x - 1.0, (2.0 * s) * n.yz);
    }
    return n;
}

vec4 ShapeOctave3(uint kind, vec4 n) {
    if (kind == ENGINE_NOISE_RIDGED) {
        float s = n.x < 0.0 ? -1.0 : 1.0;
        float r = 1.0 - s * n.x;
        return vec4(r * r, (-2.0 * r * s) * n.yzw);
    }
    if (kind == ENGINE_NOISE_BILLOW) {
        float s = n.x < 0.0 ? -1.0 : 1.0;
        return vec4(2.0 * s * n.x - 1.0, (2.0 * s) * n.yzw);
    }
    return n;
}

// --- Fractal accumulation -----------------------------------------------------------------------

// Each octave is sampled at `f * p`, so by the chain rule its gradient in world units is `f * grad`.
// The chain rule is applied before the kind transform, because that transform is a function of the
// octave value and its own derivative multiplies whatever comes in. Accumulating this alongside the
// value is what makes the result exact rather than a finite-difference estimate of the sum.
//
// Returns vec3(value, d/dx, d/dy), normalized by the sum of the amplitudes so the result keeps the
// range of a single octave.
vec3 FractalNoise2D(uint kind, vec2 p, float frequency, uint octaves, float lacunarity, float gain,
                    uint seed) {
    vec3  sum       = vec3(0.0);
    float amplitude = 1.0;
    float f         = frequency;
    float norm      = 0.0;

    for (uint o = 0u; o < octaves; ++o) {
        // Decorrelating the seed per octave avoids the octaves lining up at the lattice origin.
        vec3 n = Simplex2D(p * f, seed + o * ENGINE_HASH_PHI);
        n.yz *= f;
        vec3 shaped = ShapeOctave2(kind, n);
        sum.x += amplitude * shaped.x;
        sum.yz += amplitude * shaped.yz;
        norm += amplitude;
        amplitude *= gain;
        f *= lacunarity;
    }

    return norm > 0.0 ? sum / norm : vec3(0.0);
}

vec4 FractalNoise3D(uint kind, vec3 p, float frequency, uint octaves, float lacunarity, float gain,
                    uint seed) {
    vec4  sum       = vec4(0.0);
    float amplitude = 1.0;
    float f         = frequency;
    float norm      = 0.0;

    for (uint o = 0u; o < octaves; ++o) {
        vec4 n = Simplex3D(p * f, seed + o * ENGINE_HASH_PHI);
        n.yzw *= f;
        vec4 shaped = ShapeOctave3(kind, n);
        sum.x += amplitude * shaped.x;
        sum.yzw += amplitude * shaped.yzw;
        norm += amplitude;
        amplitude *= gain;
        f *= lacunarity;
    }

    return norm > 0.0 ? sum / norm : vec4(0.0);
}

#endif // ENGINE_LIB_NOISE_GLSL
