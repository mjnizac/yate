// Terrain.Blur: box blur over a declared radius.
//
// Class: neighbourhood, radius r. Mapping: Rn -> Rm in, the same out.
//
// Parameters:
//   0  radius      taps on each side, at least 1
//   1  components
//   2  mode        0 the mean, 1 the largest value, 2 the smallest (a dilation or an erosion), 3 the
//                  median (R2, radius at most 3: World Machine's Median blur, which keeps edges)
//   3  shape       the window over x and z (in R3 a prism of the same reach in y): 0 the square,
//                  1 the regular octagon inscribed in it, 2 the diamond, 3 the disk
//   4  radius map  1 when input 1 is an Rn -> R1 field scaling the radius per sample (clamped to
//                  [0, 1]; 0 keeps only the sample itself), read at the sample only
//   5  falloff     the structuring element of modes 1 and 2: 0 flat, 1 a cone (each tap lowered, or for
//                  the smallest value raised, by slope x distance, World Machine's Linear Slope), 2 round
//                  (a paraboloid reaching that slope at the declared radius, its Gaussian expander)
//   6  spacing     metres per sample, written by the compiler
//   7  slope       rise over run of the falloff, as float bits
//
// This is the first op that reads anything other than its own sample, so it is what makes halo
// propagation real: its input is allocated with `output halo + radius`, which is a wider border and
// therefore a different row stride, and the taps are addressed with the input's own geometry.
//
// Every tap is asserted to exist rather than clamped. Halo propagation guarantees they do, so a
// missing tap means the compiler and the kernel disagree about the radius, and silently clamping
// would hide that while changing the result at section borders -- which is exactly the kind of bug
// the seam test is meant to catch and would then not catch.

#version 460

#include "lib/kernel.lib.glsl"

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }

    uint index  = SampleIndex(local);
    int  radius = max(ParamInt(0), 1);
    uint count  = max(ParamUint(1), 1u);
    uint mode   = ParamUint(2);
    uint shape  = ParamUint(3);
    uint falloff = mode == 0u ? 0u : ParamUint(5);
    // The drop per sample of distance, in the field's units: a slope is metres over metres.
    float drop = ParamFloat(7) * ParamFloat(6);

    FloatBuffer source = FloatBuffer(pc.inputs[0]);
    FloatBuffer target = FloatBuffer(pc.outputs[0]);

    // The window's reach at this sample, in taps: the declared radius, times the map where there is one.
    float reach = float(radius);
    if (ParamUint(4) == 1u) {
        reach *= clamp(FloatBuffer(pc.inputs[1]).values[InputIndex(1u, local)], 0.0, 1.0);
    }

    // The y extent is 1 in R2, so the vertical loop collapses to the single plane.
    int spanY = IsR3() ? radius : 0;

    for (uint c = 0u; c < count; ++c) {
        if (mode == 3u) {
            // The median by counting: the value with fewer than half the window below it and at least
            // half at or below it. Order-free, so every section agrees bit for bit.
            float window[49];
            uint  n = 0u;
            for (int dz = -radius; dz <= radius; ++dz) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    float ax = abs(float(dx));
                    float az = abs(float(dz));
                    bool  inside = shape == 1u ? max(ax, az) <= reach && ax + az <= reach * 1.41421356
                                 : shape == 2u ? ax + az <= reach
                                 : shape == 3u ? ax * ax + az * az <= reach * (reach + 1.0)
                                               : max(ax, az) <= reach;
                    if (inside && n < 49u && InputContains(0u, local, ivec3(dx, 0, dz))) {
                        window[n++] = source.values[InputIndexOffset(0u, local, ivec3(dx, 0, dz)) * count + c];
                    }
                }
            }
            float median = 0.0;
            uint  middle = n / 2u;
            for (uint i = 0u; i < n; ++i) {
                uint below = 0u;
                uint equal = 0u;
                for (uint j = 0u; j < n; ++j) {
                    below += window[j] < window[i] ? 1u : 0u;
                    equal += window[j] == window[i] ? 1u : 0u;
                }
                if (below <= middle && middle < below + equal) {
                    median = window[i];
                }
            }
            target.values[index * count + c] = median;
            continue;
        }
        float sum  = 0.0;
        float taps = 0.0;
        float most = -3.402823e38;
        float least = 3.402823e38;
        for (int dy = -spanY; dy <= spanY; ++dy) {
            for (int dz = -radius; dz <= radius; ++dz) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    float ax = abs(float(dx));
                    float az = abs(float(dz));
                    bool  inside = shape == 1u ? max(ax, az) <= reach && ax + az <= reach * 1.41421356
                                 : shape == 2u ? ax + az <= reach
                                 : shape == 3u ? ax * ax + az * az <= reach * (reach + 1.0)
                                               : max(ax, az) <= reach;
                    // In R3 the window is the shape's prism, its height scaled by the map too.
                    if (!inside || abs(float(dy)) > reach) {
                        continue;
                    }
                    ivec3 offset = ivec3(dx, dy, dz);
                    if (!InputContains(0u, local, offset)) {
                        continue;
                    }
                    uint at = InputIndexOffset(0u, local, offset);
                    float v = source.values[at * count + c];
                    if (falloff != 0u) {
                        float d = length(vec3(ax, float(dy), az));
                        float fall = falloff == 1u ? drop * d : drop * d * d / (2.0 * float(radius));
                        v += mode == 1u ? -fall : fall;
                    }
                    sum += v;
                    most  = max(most, v);
                    least = min(least, v);
                    taps += 1.0;
                }
            }
        }
        float result = mode == 1u ? most : mode == 2u ? least : sum / max(taps, 1.0);
        target.values[index * count + c] = taps > 0.0 ? result : 0.0;
    }
}
