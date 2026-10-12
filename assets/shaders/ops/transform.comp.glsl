// Affine map of a position field: translation, rotation and scale in the (x, z) plane.
//
// The matrix takes world positions to the local space of whatever is evaluated at them, so a shape
// placed somewhere is the shape evaluated at positions moved the other way. Pointwise, which is what
// lets one shape be reused at many places without a second copy of it.
//
// Parameters: words 0 to 3 are the 2x2 matrix in row-major order, words 4 and 5 the translation.

#version 460

#include "lib/kernel.lib.glsl"

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }

    uint index = SampleIndex(local);
    // Through `InputIndex`: the position may be wider than this node (see `curve.comp.glsl`).
    vec2 p = LoadVec2(0u, InputIndex(0u, local));

    float x = ParamFloat(0) * p.x + ParamFloat(1) * p.y + ParamFloat(4);
    float z = ParamFloat(2) * p.x + ParamFloat(3) * p.y + ParamFloat(5);

    StoreVec2(0u, index, vec2(x, z));
}
