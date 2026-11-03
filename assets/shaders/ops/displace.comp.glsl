// Filters.Displace: a field read at its own sample moved by an offset field, World Machine's Displacement.
//
// Class: neighbourhood, radius r + 1. Mappings: `R2 -> R1` field and `R2 -> R2` offset in, `R2 -> R1` out.
//
// The offset is in metres along x and z; divided by the spacing it is a move in samples, shortened to
// `radius` samples when longer (the direction kept), and the field is read there bilinearly. The four
// taps then lie within `radius + 1` samples, which is the halo the node declares, so they always exist.
//
// Unlike a warp of the position fed into the field, this bends a field that is already finished,
// neighbourhood ops (an expander, a blur) included.
//
// Parameters:
//   0  sample spacing in metres, baked in by the compiler
//   1  radius      the longest move, in samples, at least 1

#version 460

#include "lib/kernel.lib.glsl"

float Tap(FloatBuffer source, uvec3 local, ivec2 at) {
    return source.values[InputIndexOffset(0u, local, ivec3(at.x, 0, at.y))];
}

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }

    float spacing = max(ParamFloat(0), 1e-12);
    float radius  = float(max(ParamUint(1), 1u));

    FloatBuffer source = FloatBuffer(pc.inputs[0]);
    FloatBuffer offset = FloatBuffer(pc.inputs[1]);
    FloatBuffer target = FloatBuffer(pc.outputs[0]);

    uint at   = InputIndex(1u, local) * 2u;
    vec2 move = vec2(offset.values[at], offset.values[at + 1u]) / spacing;
    float len = length(move);
    if (len > radius) {
        move *= radius / len;
    }

    ivec2 base = ivec2(floor(move));
    vec2  f    = move - vec2(base);
    float near = mix(Tap(source, local, base), Tap(source, local, base + ivec2(1, 0)), f.x);
    float far  = mix(Tap(source, local, base + ivec2(0, 1)), Tap(source, local, base + ivec2(1, 1)), f.x);
    target.values[SampleIndex(local)] = mix(near, far, f.y);
}
