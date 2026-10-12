// Erosion.Gullies: gullies cut into a heightfield across its slope, without simulating water.
//
// Class: pointwise (halo 0). Inputs: 0 the height (R2 -> R1), 1 its gradient (R2 -> R2), 2 the position
// the pattern is drawn at, in metres (R2 -> R2). Output: the height with the gullies cut (R2 -> R1).
//
// Clay John's eroded-terrain noise. Each octave is a sum of cosine stripes centred on jittered lattice
// points and blended with a Gaussian weight; a stripe varies along the contour direction, so its crests
// and troughs run down the slope. Every octave turns the next one's direction by the derivative it
// produced, which is what makes the small gullies branch off the large ones instead of running parallel.
// Flat ground gets nothing: the cut is scaled by the slope up to `full`.
//
// Parameters:
//   0  cell       size in metres of the first octave's lattice, strictly positive
//   1  octaves    count, at least 1
//   2  strength   half the depth of the first octave, in metres
//   3  full       slope at and above which the full strength applies
//   4  gain       amplitude multiplier per octave
//   5  lacunarity frequency multiplier per octave
//   6  seed salt  decorrelates nodes that share the job seed

#version 460

#include "lib/kernel.lib.glsl"
#include "lib/hash.lib.glsl"

const float kTau = 6.28318530718;

// Value, and its derivative with respect to the position, of one octave at `p` (in cells). `dir` is the
// contour direction scaled by the slope, so steeper ground gets denser stripes.
vec3 GullyOctave(vec2 p, vec2 dir, uint seed) {
    vec2  cell  = floor(p);
    vec2  local = p - cell;
    float value = 0.0;
    vec2  slope = vec2(0.0);
    float total = 0.0;
    for (int i = -2; i <= 1; ++i) {
        for (int j = -2; j <= 1; ++j) {
            vec2  offset = vec2(float(i), float(j));
            uint  h      = HashLattice2(ivec2(cell - offset), seed);
            vec2  jitter = vec2(HashToUnit(h), HashToUnit(HashMix(h ^ ENGINE_HASH_PHI)));
            vec2  d      = local + offset - jitter * 0.5;
            float weight = exp(-2.0 * dot(d, d));
            float phase  = dot(d, dir) * kTau;
            value += cos(phase) * weight;
            slope += -sin(phase) * weight * dir;
            total += weight;
        }
    }
    return vec3(value, slope) / total;
}

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }
    uint index = SampleIndex(local);

    float height   = LoadScalar(0u, InputIndex(0u, local));
    vec2  gradient = LoadVec2(1u, InputIndex(1u, local));
    vec2  position = LoadVec2(2u, InputIndex(2u, local));

    float cell       = ParamFloat(0);
    uint  octaves    = max(ParamUint(1), 1u);
    float strength   = ParamFloat(2);
    float full       = ParamFloat(3);
    float gain       = ParamFloat(4);
    float lacunarity = ParamFloat(5);
    uint  seed       = pc.seed ^ (ParamUint(6) * ENGINE_HASH_PHI);

    // The contour direction: the gradient turned a quarter.
    vec2  dir       = vec2(gradient.y, -gradient.x);
    float amplitude = 0.5;
    float frequency = 1.0;
    float sum       = 0.0;
    vec2  bend      = vec2(0.0);
    for (uint o = 0u; o < octaves; ++o) {
        vec3 e = GullyOctave(position / cell * frequency, dir + vec2(bend.y, -bend.x),
                             seed + o * ENGINE_HASH_PHI);
        sum += e.x * amplitude;
        bend += e.yz * amplitude * frequency;
        amplitude *= gain;
        frequency *= lacunarity;
    }

    float steep = clamp(length(gradient) / full, 0.0, 1.0);
    StoreScalar(0u, index, height + sum * strength * steep);
}
