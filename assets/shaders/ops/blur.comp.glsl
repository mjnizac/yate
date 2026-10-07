// Terrain.Blur: box blur over a declared radius.
//
// Class: neighbourhood, radius r. Mapping: Rn -> Rm in, the same out.
//
// Parameters:
//   0  radius      taps on each side, at least 1
//   1  components
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

    FloatBuffer source = FloatBuffer(pc.inputs[0]);
    FloatBuffer target = FloatBuffer(pc.outputs[0]);

    // The y extent is 1 in R2, so the vertical loop collapses to the single plane.
    int spanY = IsR3() ? radius : 0;

    for (uint c = 0u; c < count; ++c) {
        float sum  = 0.0;
        float taps = 0.0;
        for (int dy = -spanY; dy <= spanY; ++dy) {
            for (int dz = -radius; dz <= radius; ++dz) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    ivec3 offset = ivec3(dx, dy, dz);
                    if (!InputContains(0u, local, offset)) {
                        continue;
                    }
                    uint at = InputIndexOffset(0u, local, offset);
                    sum += source.values[at * count + c];
                    taps += 1.0;
                }
            }
        }
        target.values[index * count + c] = taps > 0.0 ? sum / taps : 0.0;
    }
}
