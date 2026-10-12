// Combine.Blend: linear interpolation between two values, driven by a scalar mask.
//
// Class: pointwise (halo 0). Mappings: a and b share Rn -> Rm, mask is Rn -> R1, output Rn -> Rm.
//
// Parameters:
//   0  components

#version 460

#include "lib/kernel.lib.glsl"

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }

    uint index = SampleIndex(local);
    // Read through `InputIndex`, not `index`: an input is allocated with *its* halo, which is the widest
    // any of its consumers needs, so it can be wider than this node's own and then its rows are
    // longer. `index` is the position in this node's own padded layout and is only right for outputs.
    uint aIndex    = InputIndex(0u, local);
    uint bIndex    = InputIndex(1u, local);
    uint maskIndex = InputIndex(2u, local);
    uint count = max(ParamUint(0), 1u);

    FloatBuffer a = FloatBuffer(pc.inputs[0]);
    FloatBuffer b = FloatBuffer(pc.inputs[1]);
    FloatBuffer mask = FloatBuffer(pc.inputs[2]);
    FloatBuffer target = FloatBuffer(pc.outputs[0]);

    // A mask outside [0, 1] would extrapolate, which is never what a blend means.
    float t = clamp(mask.values[maskIndex], 0.0, 1.0);

    for (uint c = 0u; c < count; ++c) {
        target.values[index * count + c] =
            mix(a.values[aIndex * count + c], b.values[bIndex * count + c], t);
    }
}
