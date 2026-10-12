// Coords: writes the world-space position, in metres, of every padded sample.
//
// Class: pointwise (halo 0). Mappings: R2 -> R2 and R3 -> R3.
// Parameters:
//   0  sample spacing in metres, written by the compiler from the job resolution. Zero, which is what
//      a kernel dispatched by hand gets, means "no spacing": the integer sample index comes out, which
//      is the form the compute round-trip test pins down.
//
// Besides being the primitive every procedural op builds on, this kernel is what the
// compute round-trip test checks: its output pins down the sample layout, the halo offset and
// the integer section origin at the same time.

#version 460

#include "lib/kernel.lib.glsl"

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }

    uint  index = SampleIndex(local);
    ivec3 world = WorldSample(local);

    float spacing = ParamFloat(0);
    if (spacing == 0.0) {
        spacing = 1.0;
    }

    // The integer sample coordinate times the spacing, never accumulated, so neighbouring sections
    // agree exactly on their shared samples.
    if (IsR3()) {
        StoreVec3(0u, index, vec3(world) * spacing);
    } else {
        StoreVec2(0u, index, vec2(float(world.x), float(world.z)) * spacing);
    }
}
