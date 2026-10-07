// Thermal erosion: material slides downhill wherever the local slope passes the talus angle.
//
// Class: iterative. The evaluator runs this kernel once per iteration over a ping-ponged pair of
// buffers, so the shader itself knows nothing about iteration beyond reading the index for its Tracy
// zone and for nothing else. One iteration moves material at most one cell, which is why the op
// declares an influence radius equal to its iteration count and the compiler turns that into the halo.
//
// The scheme is the classic one, written so it is symmetric and conservative:
//
//   for each neighbour n with d = h(c) - h(n) > talus:   move strength * (d - talus) / k
//
// where k is the number of such neighbours. Every cell computes what it gives away and what each
// neighbour would give it, from the same heights, so no sample depends on the order cells are visited
// and the result is identical however the work is split across workgroups or sections. That is what
// makes it safe to run in place on a ping-ponged pair rather than needing an atomic scatter.
//
// Conservation: a cell loses `strength * sum((d_i - talus)) / k` and each neighbour gains its own
// share of the same quantity, recomputed from the same two heights, so the total is preserved up to
// floating-point rounding. Nothing is created at a section border either, because a border sample
// reads real neighbours out of the halo rather than a clamped copy of itself.
//
// Parameters:
//   0  talus, as a height difference per cell
//   1  strength, the fraction of the excess moved per iteration
//   9  iteration index, written by the evaluator (unused here, reserved by the interface)

#version 460

#include "lib/kernel.lib.glsl"

// The four axis neighbours. Diagonals are deliberately left out: including them needs a 1/sqrt(2)
// weight to stay isotropic, and with it the stable strength drops, so the same visual result costs
// more iterations and therefore more halo.
const ivec2 kNeighbours2[4] = ivec2[4](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));
const ivec3 kNeighbours3[6] = ivec3[6](ivec3(1, 0, 0), ivec3(-1, 0, 0), ivec3(0, 1, 0),
                                       ivec3(0, -1, 0), ivec3(0, 0, 1), ivec3(0, 0, -1));

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }

    float talus    = ParamFloat(0);
    float strength = ParamFloat(1);

    FloatBuffer source = FloatBuffer(pc.inputs[0]);
    FloatBuffer target = FloatBuffer(pc.outputs[0]);

    uint  centreIn = InputIndex(0, local);
    float centre   = source.values[centreIn];

    // What this cell gives away, and what its neighbours give it. Both from the same heights.
    float given    = 0.0;
    float received = 0.0;

    if (IsR3()) {
        for (uint n = 0u; n < 6u; ++n) {
            ivec3 offset = kNeighbours3[n];
            if (!InputContains(0, local, offset)) {
                continue;
            }
            float neighbour = source.values[InputIndexOffset(0, local, offset)];

            float down = centre - neighbour;
            if (down > talus) {
                given += down - talus;
            }
            float up = neighbour - centre;
            if (up > talus) {
                received += up - talus;
            }
        }
        // Normalising by the full neighbour count rather than by the number that qualified keeps the
        // amount a cell gives away bounded by `strength` times its steepest drop, which is the
        // condition the strength limit in the builder is derived from.
        target.values[SampleIndex(local)] = centre + strength * (received - given) / 6.0;
    } else {
        for (uint n = 0u; n < 4u; ++n) {
            ivec3 offset = ivec3(kNeighbours2[n].x, 0, kNeighbours2[n].y);
            if (!InputContains(0, local, offset)) {
                continue;
            }
            float neighbour = source.values[InputIndexOffset(0, local, offset)];

            float down = centre - neighbour;
            if (down > talus) {
                given += down - talus;
            }
            float up = neighbour - centre;
            if (up > talus) {
                received += up - talus;
            }
        }
        target.values[SampleIndex(local)] = centre + strength * (received - given) / 4.0;
    }
}
