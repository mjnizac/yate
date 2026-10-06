// Coords: writes the integer world-space sample coordinate of every padded sample.
//
// Class: pointwise (halo 0). Mappings: R2 -> R2 and R3 -> R3.
// Parameters: none.
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

    if (IsR3()) {
        StoreVec3(0u, index, vec3(world));
    } else {
        StoreVec2(0u, index, vec2(float(world.x), float(world.z)));
    }
}
