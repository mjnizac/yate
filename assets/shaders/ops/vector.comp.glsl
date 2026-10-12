// Component extraction and assembly.
//
// Class: pointwise (halo 0).
// Variant: specialization constant 3 selects the operation.
//   Extract: Rn -> Rm in, Rn -> R1 out
//   Combine: 2 to 4 Rn -> R1 inputs, Rn -> Rm out
//
// Parameters, Extract:
//   0  component index
//   1  components of the input
// Parameters, Combine:
//   0  number of components to assemble

#version 460

#include "lib/kernel.lib.glsl"

layout(constant_id = 3) const uint kVectorOp = 0;

#define VECTOR_EXTRACT 0u
#define VECTOR_COMBINE 1u

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }

    uint index = SampleIndex(local);
    // Read through `InputIndex`, not `index`: an input is allocated with *its* halo, which is the widest
    // any of its consumers needs, so it can be wider than this node's own and then its rows are
    // longer. `index` is the position in this node's own padded layout and is only right for outputs.

    if (kVectorOp == VECTOR_EXTRACT) {
        uint component = ParamUint(0);
        uint count = max(ParamUint(1), 1u);
        FloatBuffer source = FloatBuffer(pc.inputs[0]);
        StoreScalar(0u, index, source.values[InputIndex(0u, local) * count + component]);
        return;
    }

    uint count = clamp(ParamUint(0), 2u, 4u);
    FloatBuffer target = FloatBuffer(pc.outputs[0]);
    for (uint c = 0u; c < count; ++c) {
        target.values[index * count + c] =
            FloatBuffer(pc.inputs[c]).values[InputIndex(c, local)];
    }
}
