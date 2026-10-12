// Pointwise curve: clamp, linear remap, power, smoothstep.
//
// Class: pointwise (halo 0). Mapping: Rn -> Rm in, the same out.
// Variant: specialization constant 3 selects the curve.
//
// Parameters:
//   0  a   lower bound (Clamp, Smoothstep), output minimum (Remap), exponent (Power)
//   1  b   upper bound (Clamp, Smoothstep), output maximum (Remap)
//   2  components

#version 460

#include "lib/kernel.lib.glsl"

layout(constant_id = 3) const uint kCurveOp = 0;

#define CURVE_CLAMP      0u
#define CURVE_REMAP      1u
#define CURVE_POWER      2u
#define CURVE_SMOOTHSTEP 3u

float Apply(float v, float a, float b) {
    switch (kCurveOp) {
        case CURVE_CLAMP: return clamp(v, a, b);
        // Remap takes the canonical [-1, 1] band a noise op produces into [a, b].
        case CURVE_REMAP: return a + (b - a) * (v * 0.5 + 0.5);
        // The sign is preserved so a signed field stays signed under an even exponent.
        case CURVE_POWER: return sign(v) * pow(abs(v), max(a, 0.0001));
        default: return smoothstep(a, b, v);
    }
}

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }

    uint index   = SampleIndex(local);
    // Read through `InputIndex`, not `index`: an input is allocated with *its* halo, which is the widest
    // any of its consumers needs, so it can be wider than this node's own and then its rows are
    // longer. `index` is the position in this node's own padded layout and is only right for outputs.
    uint inIndex = InputIndex(0u, local);
    uint count = max(ParamUint(2), 1u);
    float a = ParamFloat(0);
    float b = ParamFloat(1);

    FloatBuffer source = FloatBuffer(pc.inputs[0]);
    FloatBuffer target = FloatBuffer(pc.outputs[0]);

    for (uint c = 0u; c < count; ++c) {
        target.values[index * count + c] = Apply(source.values[inIndex * count + c], a, b);
    }
}
