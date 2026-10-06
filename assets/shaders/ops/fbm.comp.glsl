// Noises.fBm: fractal noise with its analytic gradient, over any base function.
//
// Class: pointwise (halo 0).
// Mappings: channel 0 is R2 -> R1 or R3 -> R1 (the value), channel 1 is R2 -> R2 or R3 -> R3
//           (the gradient).
//
// Variants (specialization constants, so both fold away when the pipeline is created):
//   3  base function, see ENGINE_BASE_* in lib/noise.lib.glsl
//   4  normalize: 1 divides by the sum of the octave amplitudes, 0 is the classic unnormalized sum
//
// Parameters:
//   0  frequency    cycles per metre, strictly positive
//   1  octaves      count, at least 1
//   2  lacunarity   frequency multiplier per octave, strictly positive
//   3  persistence  amplitude multiplier per octave, strictly positive
//   4  amplitude    output scale, in metres, unbounded
//   5  offset       output offset, in metres
//   6  resolution   metres per sample
//   7  seed salt    decorrelates nodes that share the job seed
//
// This file is the only place that knows which base functions exist. The algorithm in
// lib/fbm.lib.glsl names none of them.

#version 460

#include "lib/kernel.lib.glsl"
#include "lib/noise.lib.glsl"

layout(constant_id = 3) const uint kBaseFunction = ENGINE_BASE_SIMPLEX;
layout(constant_id = 4) const uint kFbmNormalize = 1u;

#include "lib/fbm.lib.glsl"

ENGINE_DEFINE_FBM(Fbm2Simplex, vec2, vec3, FbmWeight2, Simplex2D)
ENGINE_DEFINE_FBM(Fbm2Ridged, vec2, vec3, FbmWeight2, Ridged2D)
ENGINE_DEFINE_FBM(Fbm2Billow, vec2, vec3, FbmWeight2, Billow2D)

ENGINE_DEFINE_FBM(Fbm3Simplex, vec3, vec4, FbmWeight3, Simplex3D)
ENGINE_DEFINE_FBM(Fbm3Ridged, vec3, vec4, FbmWeight3, Ridged3D)
ENGINE_DEFINE_FBM(Fbm3Billow, vec3, vec4, FbmWeight3, Billow3D)

#define CHANNEL_VALUE    0u
#define CHANNEL_GRADIENT 1u

// `kBaseFunction` is a specialization constant, so exactly one branch survives in the pipeline.
vec3 Fbm2(vec2 p, uint octaves, float amplitude, float persistence, float frequency,
          float lacunarity, uint seed) {
    if (kBaseFunction == ENGINE_BASE_RIDGED) {
        return Fbm2Ridged(p, octaves, amplitude, persistence, frequency, lacunarity, seed);
    }
    if (kBaseFunction == ENGINE_BASE_BILLOW) {
        return Fbm2Billow(p, octaves, amplitude, persistence, frequency, lacunarity, seed);
    }
    return Fbm2Simplex(p, octaves, amplitude, persistence, frequency, lacunarity, seed);
}

vec4 Fbm3(vec3 p, uint octaves, float amplitude, float persistence, float frequency,
          float lacunarity, uint seed) {
    if (kBaseFunction == ENGINE_BASE_RIDGED) {
        return Fbm3Ridged(p, octaves, amplitude, persistence, frequency, lacunarity, seed);
    }
    if (kBaseFunction == ENGINE_BASE_BILLOW) {
        return Fbm3Billow(p, octaves, amplitude, persistence, frequency, lacunarity, seed);
    }
    return Fbm3Simplex(p, octaves, amplitude, persistence, frequency, lacunarity, seed);
}

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }

    uint  index = SampleIndex(local);
    ivec3 world = WorldSample(local);

    float frequency   = ParamFloat(0);
    uint  octaves     = max(ParamUint(1), 1u);
    float lacunarity  = ParamFloat(2);
    float persistence = ParamFloat(3);
    float amplitude   = ParamFloat(4);
    float offset      = ParamFloat(5);
    float resolution  = ParamFloat(6);
    uint  seed        = pc.seed ^ (ParamUint(7) * ENGINE_HASH_PHI);

    if (IsR3()) {
        // Positions come from the integer sample times the sample spacing, never accumulated, so
        // neighbouring sections agree exactly on their shared samples.
        vec3 position = vec3(world) * resolution;
        vec4 n = Fbm3(position, octaves, amplitude, persistence, frequency, lacunarity, seed);

        if (WantsChannel(CHANNEL_VALUE)) {
            StoreScalar(CHANNEL_VALUE, index, n.x + offset);
        }
        if (WantsChannel(CHANNEL_GRADIENT)) {
            StoreVec3(CHANNEL_GRADIENT, index, n.yzw);
        }
    } else {
        vec2 position = vec2(float(world.x), float(world.z)) * resolution;
        vec3 n = Fbm2(position, octaves, amplitude, persistence, frequency, lacunarity, seed);

        if (WantsChannel(CHANNEL_VALUE)) {
            StoreScalar(CHANNEL_VALUE, index, n.x + offset);
        }
        if (WantsChannel(CHANNEL_GRADIENT)) {
            StoreVec2(CHANNEL_GRADIENT, index, n.yz);
        }
    }
}
