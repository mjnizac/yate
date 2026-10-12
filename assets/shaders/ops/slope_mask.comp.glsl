// Masks.Slope: mask from the magnitude of a heightfield gradient.
//
// Class: pointwise (halo 0). Mapping: R2 -> R2 in (the gradient), R2 -> R1 out.
//
// Parameters:
//   0  min  slope at or below which the mask is 0
//   1  max  slope at or above which the mask is 1
//
// The slope is |grad h|, which the producing op already computed analytically. A finite-difference
// slope would need a halo and a neighbour read; this needs neither.

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
    vec2 gradient = LoadVec2(0u, InputIndex(0u, local));
    float slope = length(gradient);

    StoreScalar(0u, index, smoothstep(ParamFloat(0), ParamFloat(1), slope));
}
