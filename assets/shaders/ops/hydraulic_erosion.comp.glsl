// Hydraulic erosion: rain, flow, sediment transport and evaporation, one iteration per dispatch.
//
// Class: iterative. The evaluator runs this kernel once per iteration over a ping-ponged pair of
// buffers and tells it which iteration it is on, which is the only thing the shader needs to know to
// pick what it reads and what it writes:
//
//   first iteration  reads an `Rn -> R1` height and seeds the state from it
//   last iteration   writes an `Rn -> R1` height, and the water and sediment to channel 1 when asked
//   in between       reads and writes the three-component state
//
// The state is (height, water, sediment). One kernel rather than one per phase, because the phases of
// one iteration are local to a cell and its four neighbours, and splitting them would mean a dispatch
// and a barrier per phase for no gain.
//
// **Why the transfers are pairwise.** A section must produce bit-identical interiors however the work
// is split, which means no sample may depend on the order cells are visited. The usual outflow scheme
// normalises a cell's outflow by its own total, so a cell can only know what it *receives* by
// recomputing a neighbour's total, which needs that neighbour's neighbours: a two-cell radius, and
// therefore twice the halo per iteration. Instead each transfer here is a function of one pair alone:
//
//   flow(c -> n) = rate * min(water(c), max(0, surface(c) - surface(n)))
//
// Both cells compute that same expression from the same previous state, so what one sends is exactly
// what the other receives, the scheme is conservative, and the radius stays at one cell per iteration.
// With `rate <= 0.25` the four outflows cannot exceed the water present, which is what keeps it stable
// without a normalisation step.
//
// Parameters:
//   0  rain added per iteration
//   1  evaporation, the fraction of water lost per iteration
//   2  sediment capacity per unit of flow and slope
//   3  erosion rate, how fast a deficit in sediment is taken from the bed
//   4  deposition rate, how fast an excess is laid back down
//   5  flow rate, the fraction of a surface drop that moves per iteration
//   8  iteration count, written by the evaluator
//   9  iteration index, written by the evaluator

#version 460

#include "lib/kernel.lib.glsl"

const ivec3 kNeighbours[4] =
    ivec3[4](ivec3(1, 0, 0), ivec3(-1, 0, 0), ivec3(0, 0, 1), ivec3(0, 0, -1));

// Below this much water a cell has nothing to carry sediment with, and dividing by it would amplify
// rounding into the concentration.
const float kMinimumWater = 1e-6;

struct Cell {
    float height;
    float water;
    float sediment;
};

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }

    float rain        = ParamFloat(0);
    float evaporation = ParamFloat(1);
    float capacity    = ParamFloat(2);
    float erosionRate = ParamFloat(3);
    float deposition  = ParamFloat(4);
    float flowRate    = ParamFloat(5);

    uint  iterations = ParamUint(8);
    uint  iteration  = ParamUint(9);
    bool  first      = iteration == 0u;
    bool  last       = iteration + 1u == iterations;

    FloatBuffer source = FloatBuffer(pc.inputs[0]);
    FloatBuffer target = FloatBuffer(pc.outputs[0]);

    // The first iteration seeds the state from a plain height field, so its input has one component
    // where every later one has three.
    uint  stride = first ? 1u : 3u;
    uint  centreIn = InputIndex(0, local) * stride;

    Cell centre;
    centre.height   = source.values[centreIn];
    centre.water    = first ? 0.0 : source.values[centreIn + 1u];
    centre.sediment = first ? 0.0 : source.values[centreIn + 2u];

    // Rain falls before anything moves, so the first iteration has water to work with.
    centre.water += rain;

    float centreSurface = centre.height + centre.water;

    // What this cell sends and what its neighbours send it, both from the same previous state. The
    // drop the outflow descends is tracked at the same time, because the sediment capacity depends on
    // it, and so is the drop to the lowest neighbour, which is what bounds how deep one iteration may
    // cut.
    float waterOut    = 0.0;
    float waterIn     = 0.0;
    float sedimentOut = 0.0;
    float sedimentIn  = 0.0;
    float steepest    = 0.0;
    float lowest      = centre.height;

    for (uint n = 0u; n < 4u; ++n) {
        ivec3 offset = kNeighbours[n];
        if (!InputContains(0, local, offset)) {
            continue;
        }
        uint  at = InputIndexOffset(0, local, offset) * stride;
        Cell  neighbour;
        neighbour.height   = source.values[at];
        neighbour.water    = first ? 0.0 : source.values[at + 1u];
        neighbour.sediment = first ? 0.0 : source.values[at + 2u];
        neighbour.water += rain;

        float neighbourSurface = neighbour.height + neighbour.water;

        float down = centreSurface - neighbourSurface;
        lowest     = min(lowest, neighbour.height);

        if (down > 0.0) {
            float moved = flowRate * min(centre.water, down);
            waterOut += moved;
            // The slope that sets the capacity is the one the water is going *down*. Taking the largest
            // absolute difference to any neighbour instead, uphill ones included, gives a cell at the
            // foot of a slope the capacity of the slope above it: it erodes although nothing descends
            // there, digging a pit, which steepens the drop into it, which erodes harder. That feedback
            // is what made the op diverge for any rainfall large enough to do visible work, so the only
            // settings that stayed stable were the ones that did nothing.
            steepest = max(steepest, centre.height - neighbour.height);
            // Sediment travels at the concentration of the water that carries it.
            sedimentOut += moved * centre.sediment / max(centre.water, kMinimumWater);
        } else if (down < 0.0) {
            float moved = flowRate * min(neighbour.water, -down);
            waterIn += moved;
            sedimentIn += moved * neighbour.sediment / max(neighbour.water, kMinimumWater);
        }
    }

    Cell next;
    next.water    = centre.water - waterOut + waterIn;
    next.sediment = centre.sediment - sedimentOut + sedimentIn;
    next.height   = centre.height;

    // How much sediment the flow through this cell can hold. Flow rather than standing water, because
    // a still pond carries nothing however deep it is.
    float flow    = max(waterOut, waterIn);
    float holds   = capacity * flow * steepest;

    if (next.sediment < holds) {
        // Room to spare: take the difference out of the bed. Never more than half the drop to the
        // lowest neighbour, so one iteration cannot cut the cell below what surrounds it — without that
        // bound the capacity term alone does not stop a cell from digging past its own outlet, and the
        // next iteration sees a steeper drop and takes more.
        float taken = min(erosionRate * (holds - next.sediment),
                          0.5 * max(centre.height - lowest, 0.0));
        next.height -= taken;
        next.sediment += taken;
    } else {
        float dropped = deposition * (next.sediment - holds);
        next.height += dropped;
        next.sediment -= dropped;
    }

    // Evaporation last, so sediment left behind by water that disappeared is deposited on the next
    // iteration rather than vanishing with it.
    next.water *= 1.0 - evaporation;

    // `out` is a GLSL keyword.
    uint at = SampleIndex(local);
    if (last) {
        // The terrain is channel 0. The water and the sediment are channel 1, and only written when a
        // consumer asked for them: a script that only wants a height should not pay for a second buffer
        // or the bandwidth to fill it, which is exactly what the channel mask is for.
        if (WantsChannel(0)) {
            target.values[at] = next.height;
        }
        if (WantsChannel(1)) {
            FloatBuffer flow = FloatBuffer(pc.outputs[1]);
            flow.values[at * 2u]      = next.water;
            flow.values[at * 2u + 1u] = next.sediment;
        }
    } else {
        target.values[at * 3u]      = next.height;
        target.values[at * 3u + 1u] = next.water;
        target.values[at * 3u + 2u] = next.sediment;
    }
}
