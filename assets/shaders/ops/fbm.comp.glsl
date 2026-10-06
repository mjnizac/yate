// Noises.fBm: fractal noise with its analytic gradient.
//
// Class: pointwise (halo 0).
// Mappings: channel 0 is R2 -> R1 or R3 -> R1 (the value), channel 1 is R2 -> R2 or R3 -> R3
//           (the gradient).
// Variant: specialization constant 3 selects the noise kind.
//
// Parameters:
//   0  frequency   cycles per metre
//   1  octaves     count, at least 1
//   2  lacunarity  frequency multiplier per octave
//   3  gain        amplitude multiplier per octave
//   4  amplitude   output scale, in metres
//   5  offset      output offset, in metres
//   6  resolution  metres per sample
//   7  seed salt   decorrelates nodes that share the job seed
//
// The gradient channel is in metres per metre, so a consumer such as Terrain.Normals needs no
// finite differences and no extra dispatch.

#version 460

#include "lib/kernel.lib.glsl"
#include "lib/noise.lib.glsl"

layout(constant_id = 3) const uint kNoiseKind = 0;

#define CHANNEL_VALUE    0u
#define CHANNEL_GRADIENT 1u

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }

    uint  index = SampleIndex(local);
    ivec3 world = WorldSample(local);

    float frequency  = ParamFloat(0);
    uint  octaves    = max(ParamUint(1), 1u);
    float lacunarity = ParamFloat(2);
    float gain       = ParamFloat(3);
    float amplitude  = ParamFloat(4);
    float offset     = ParamFloat(5);
    float resolution = ParamFloat(6);
    uint  seed       = pc.seed ^ (ParamUint(7) * ENGINE_HASH_PHI);

    if (IsR3()) {
        // Positions come from the integer sample times the sample spacing, never accumulated,
        // so neighbouring sections agree exactly on their shared samples.
        vec3 position = vec3(world) * resolution;
        vec4 n = FractalNoise3D(kNoiseKind, position, frequency, octaves, lacunarity, gain, seed);

        if (WantsChannel(CHANNEL_VALUE)) {
            StoreScalar(CHANNEL_VALUE, index, n.x * amplitude + offset);
        }
        if (WantsChannel(CHANNEL_GRADIENT)) {
            StoreVec3(CHANNEL_GRADIENT, index, n.yzw * amplitude);
        }
    } else {
        vec2 position = vec2(float(world.x), float(world.z)) * resolution;
        vec3 n = FractalNoise2D(kNoiseKind, position, frequency, octaves, lacunarity, gain, seed);

        if (WantsChannel(CHANNEL_VALUE)) {
            StoreScalar(CHANNEL_VALUE, index, n.x * amplitude + offset);
        }
        if (WantsChannel(CHANNEL_GRADIENT)) {
            StoreVec2(CHANNEL_GRADIENT, index, n.yz * amplitude);
        }
    }
}
