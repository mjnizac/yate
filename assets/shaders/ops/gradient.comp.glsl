// Gradient of a scalar field by central differences.
//
// Class: neighbourhood, radius 1. Mappings: `Rn -> R1` in, `Rn -> Rn` out.
//
// This is the one op in the engine that is not analytic, and it exists because for a field that came
// out of a chain of blends, curves and erosion there is no closed form to differentiate: the only thing
// left is to measure it. Everything that *has* an analytic derivative still carries one, and a noise
// node's `.gradient` channel remains the exact one, out of the same evaluation as its value. Reach for
// this only when the field has no better answer.
//
// The estimate is the standard second-order central difference,
//
//   df/dx = (f(x + h) - f(x - h)) / (2h)
//
// with `h` the sample spacing, so the result is in units of height per metre and matches what the
// analytic channels produce. At the border of its own output both taps exist, because halo propagation
// gave the input this node's halo plus one.
//
// Parameters:
//   0  sample spacing in metres, baked in by the compiler

#version 460

#include "lib/kernel.lib.glsl"

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }

    float spacing = ParamFloat(0);
    // A zero spacing would divide by zero. It cannot happen through the builder, which rejects a
    // non-positive resolution, so this is the floor rather than a policy.
    float scale   = 1.0 / (2.0 * max(spacing, 1e-12));

    FloatBuffer source = FloatBuffer(pc.inputs[0]);
    FloatBuffer target = FloatBuffer(pc.outputs[0]);

    uint at = SampleIndex(local);

    if (IsR3()) {
        float dx = source.values[InputIndexOffset(0, local, ivec3(1, 0, 0))]
                   - source.values[InputIndexOffset(0, local, ivec3(-1, 0, 0))];
        float dy = source.values[InputIndexOffset(0, local, ivec3(0, 1, 0))]
                   - source.values[InputIndexOffset(0, local, ivec3(0, -1, 0))];
        float dz = source.values[InputIndexOffset(0, local, ivec3(0, 0, 1))]
                   - source.values[InputIndexOffset(0, local, ivec3(0, 0, -1))];
        target.values[at * 3u]      = dx * scale;
        target.values[at * 3u + 1u] = dy * scale;
        target.values[at * 3u + 2u] = dz * scale;
    } else {
        float dx = source.values[InputIndexOffset(0, local, ivec3(1, 0, 0))]
                   - source.values[InputIndexOffset(0, local, ivec3(-1, 0, 0))];
        float dz = source.values[InputIndexOffset(0, local, ivec3(0, 0, 1))]
                   - source.values[InputIndexOffset(0, local, ivec3(0, 0, -1))];
        target.values[at * 2u]      = dx * scale;
        target.values[at * 2u + 1u] = dz * scale;
    }
}
