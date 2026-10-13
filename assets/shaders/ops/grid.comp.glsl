// Terrain.Grid: a field given as a regular grid of values, sampled at a position field.
//
// Class: pointwise (halo 0). Input 0 is the position in metres (R2 -> R2); input slot 1 is the grid,
// bound by the evaluator: every grid of the graph, back to back. Output R2 -> R1.
//
// Catmull-Rom in both axes, so the field passes through every value and its slope is continuous
// between them. Outside the grid the edge is held. A function of the position alone, so sections
// agree on their borders by construction.
//
// Parameters:
//   0  offset    of this grid's first value in the buffer
//   1  width     values along x
//   2  depth     values along z
//   3  origin x  world position of value (0, 0), metres
//   4  origin z
//   5  cell      metres between values

#version 460

#include "lib/kernel.lib.glsl"

float GridAt(FloatBuffer grid, uint offset, uint width, uint depth, int x, int z) {
    x = clamp(x, 0, int(width) - 1);
    z = clamp(z, 0, int(depth) - 1);
    return grid.values[offset + uint(z) * width + uint(x)];
}

float CatmullRom(float a, float b, float c, float d, float t) {
    return b + 0.5 * t * (c - a + t * (2.0 * a - 5.0 * b + 4.0 * c - d + t * (3.0 * (b - c) + d - a)));
}

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }
    uint index    = SampleIndex(local);
    vec2 position = LoadVec2(0u, InputIndex(0u, local));

    FloatBuffer grid   = FloatBuffer(pc.inputs[1]);
    uint        offset = ParamUint(0);
    uint        width  = ParamUint(1);
    uint        depth  = ParamUint(2);
    vec2        origin = vec2(ParamFloat(3), ParamFloat(4));
    float       cell   = ParamFloat(5);

    vec2  g = clamp((position - origin) / cell, vec2(0.0), vec2(float(width - 1u), float(depth - 1u)));
    ivec2 i = ivec2(floor(g));
    vec2  t = g - vec2(i);

    float rows[4];
    for (int j = -1; j <= 2; ++j) {
        rows[j + 1] = CatmullRom(GridAt(grid, offset, width, depth, i.x - 1, i.y + j),
                                 GridAt(grid, offset, width, depth, i.x, i.y + j),
                                 GridAt(grid, offset, width, depth, i.x + 1, i.y + j),
                                 GridAt(grid, offset, width, depth, i.x + 2, i.y + j), t.x);
    }
    StoreScalar(0u, index, CatmullRom(rows[0], rows[1], rows[2], rows[3], t.y));
}
