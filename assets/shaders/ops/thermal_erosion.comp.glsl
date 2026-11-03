// Thermal erosion: material slides downhill wherever the local slope passes the talus angle.
//
// Class: iterative. The evaluator runs this kernel once per iteration over a ping-ponged pair of
// buffers. One iteration moves material at most one cell, which is why the op declares an influence
// radius equal to its iteration count and the compiler turns that into the halo.
//
// The scheme is the classic one, written so it is symmetric and conservative:
//
//   for each neighbour n with d = h(c) - h(n) > talus:   move strength * (d - talus) / k
//
// where k is the number of neighbours. Every cell computes what it gives away and what each
// neighbour would give it, from the same heights, so no sample depends on the order cells are visited
// and the result is identical however the work is split across workgroups or sections. That is what
// makes it safe to run in place on a ping-ponged pair rather than needing an atomic scatter.
//
// Conservation: a cell loses `strength * sum((d_i - talus)) / k` and each neighbour gains its own
// share of the same quantity, recomputed from the same two heights, so the total is preserved up to
// floating-point rounding. Nothing is created at a section border either, because a border sample
// reads real neighbours out of the halo rather than a clamped copy of itself.
//
// World Machine's Thermal Weathering adds three things, all here:
//
// * **Mass balance** b in [0, 1] weights the two sides of every transfer: what a cell gives is scaled by
//   2 (1 - b) and what it receives by 2 b. At 0.5 both are 1 and the scheme is the conservative one,
//   bit for bit; at 0 cells only wear (the forum's "mass balance all the way left" that carves valleys
//   and water gaps); at 1 they only fill, which builds a constant-slope apron at the talus angle (the
//   forum's constant-slope expander).
// * **Two-phase**: the state is (rock, talus) and the surface is their sum. Rock steeper than `rock`
//   against a neighbour's surface breaks into talus in place; only talus moves, at the talus angle, and
//   a cell can send at most its own talus. With the talus angle just under the rock angle the rock
//   faces stay planar and the talus collects at their foot. The talus depth is channel 1.
// * **Strength map**: input 1, clamped to [0, 1], scales the strength per sample (talus production). A
//   transfer uses the sender's strength, read by both sides, so it stays conservative.
//
// Parameters:
//   0  talus, as a slope: rise over run, so tan of the angle (0.7 is about 35 degrees)
//   1  strength, the fraction of the excess moved per iteration
//   2  sample spacing in metres, written by the compiler from the job resolution
//   3  mass balance in [0, 1]
//   4  1 for two-phase
//   5  rock, the slope above which rock breaks into talus (two-phase only)
//   6  1 when input 1 is a strength map
//   8  iteration count, written by the evaluator
//   9  iteration index, written by the evaluator

#version 460

#include "lib/kernel.lib.glsl"

// The axis neighbours. Diagonals are deliberately left out: including them needs a 1/sqrt(2) weight
// to stay isotropic, and with it the stable strength drops, so the same visual result costs more
// iterations and therefore more halo. The orders are the ones the sums have always been taken in, so a
// default run is bit for bit what it was before the mass balance and the map existed.
const ivec3 kNeighbours2[4] = ivec3[4](ivec3(1, 0, 0), ivec3(-1, 0, 0), ivec3(0, 0, 1), ivec3(0, 0, -1));
const ivec3 kNeighbours3[6] = ivec3[6](ivec3(1, 0, 0), ivec3(-1, 0, 0), ivec3(0, 1, 0),
                                       ivec3(0, -1, 0), ivec3(0, 0, 1), ivec3(0, 0, -1));

float StrengthAt(ivec3 offset, float strength) {
    if (ParamUint(6) == 0u) {
        return strength;
    }
    // The map has the halo the op's radius asks for, which the input may exceed when another consumer
    // widened it; past the map's edge the sample's own value stands in. That ring is outside what any
    // iteration's result keeps, so it changes nothing that is read.
    uvec3 local = LocalSample();
    ivec3 at    = InputContains(1u, local, offset) ? offset : ivec3(0);
    return strength * clamp(FloatBuffer(pc.inputs[1]).values[InputIndexOffset(1u, local, at)], 0.0, 1.0);
}

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }

    // The talus is a slope, so the height difference a cell may hold against its neighbour is the
    // slope times the distance to it. Every neighbour here is an axis neighbour, one sample away.
    float spacing  = max(ParamFloat(2), 1e-6);
    float talus    = ParamFloat(0) * spacing;
    float rock     = ParamFloat(5) * spacing;
    float balance  = ParamFloat(3);
    float give     = 2.0 * (1.0 - balance);
    float take     = 2.0 * balance;
    bool  twoPhase = ParamUint(4) == 1u;
    bool  first    = ParamUint(9) == 0u;
    bool  last     = ParamUint(9) + 1u == ParamUint(8);
    uint  count    = IsR3() ? 6u : 4u;

    FloatBuffer source = FloatBuffer(pc.inputs[0]);
    FloatBuffer target = FloatBuffer(pc.outputs[0]);

    // The first iteration reads the plain height; a two-phase state is (rock, talus) after it.
    uint  stride   = twoPhase && !first ? 2u : 1u;
    uint  centreIn = InputIndex(0, local) * stride;
    float centre   = source.values[centreIn];
    float scree    = stride == 2u ? source.values[centreIn + 1u] : 0.0;
    float mine     = StrengthAt(ivec3(0), ParamFloat(1));
    // Without a map every transfer has the same strength, applied once at the end as it always was.
    bool  mapped   = ParamUint(6) == 1u;

    // What this cell gives away, and what its neighbours give it. Both from the same state.
    float given    = 0.0;
    float received = 0.0;
    float broken   = 0.0;

    for (uint n = 0u; n < count; ++n) {
        ivec3 offset = IsR3() ? kNeighbours3[n] : kNeighbours2[n];
        if (!InputContains(0, local, offset)) {
            continue;
        }
        uint  at        = InputIndexOffset(0, local, offset) * stride;
        float neighbour = source.values[at];
        float theirs    = stride == 2u ? source.values[at + 1u] : 0.0;
        float down      = centre + scree - neighbour - theirs;
        if (twoPhase) {
            // Only talus moves, and no more than the sender holds: each side reads the sender's talus.
            given    += min(mine * max(down - talus, 0.0), scree / float(count));
            received += min(StrengthAt(offset, ParamFloat(1)) * max(-down - talus, 0.0), theirs / float(count));
            broken   += max(centre - neighbour - theirs - rock, 0.0);
        } else {
            given    += (mapped ? mine : 1.0) * max(down - talus, 0.0);
            received += (mapped ? StrengthAt(offset, ParamFloat(1)) : 1.0) * max(-down - talus, 0.0);
        }
    }

    uint at = SampleIndex(local);
    if (!twoPhase) {
        // Normalising by the full neighbour count rather than by the number that qualified keeps the
        // amount a cell gives away bounded by `strength` times its steepest drop, which is the
        // condition the strength limit in the builder is derived from.
        target.values[at] = centre + (mapped ? 1.0 : ParamFloat(1)) * (take * received - give * given) / float(count);
        return;
    }

    // Rock that breaks becomes talus in place; the mass balance decides how much of each survives.
    broken *= mine / float(count);
    float nextRock  = centre - broken * min(give, 1.0);
    float nextScree = max(scree + received - given + broken * min(take, 1.0), 0.0);
    if (last) {
        if (WantsChannel(0)) {
            target.values[at] = nextRock + nextScree;
        }
        if (WantsChannel(1)) {
            FloatBuffer(pc.outputs[1]).values[at] = nextScree;
        }
    } else {
        target.values[at * 2u]      = nextRock;
        target.values[at * 2u + 1u] = nextScree;
    }
}
