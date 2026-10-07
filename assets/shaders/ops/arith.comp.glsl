// Pointwise arithmetic over any mapping, with broadcasting.
//
// Class: pointwise (halo 0). Mappings: Rn -> Rm when both operands match, or Rn -> Rm against
// Rn -> R1, which broadcasts the scalar side (spec section 8).
// Variant: specialization constant 3 selects the operation, 4 says whether an operand is an
// immediate instead of a buffer: 0 for neither, 1 for the left one, 2 for the right one.
//
// A constant operand used to arrive as a whole section buffer filled with one repeated number, written
// by its own dispatch. The compiler now folds such a producer into this kernel, which saves that
// dispatch; the immediate is a scalar, so it broadcasts over any component count the same way a
// one-component operand does.
//
// Parameters:
//   0  components of input 0
//   1  components of input 1
//   2  components of the output
//   3  the immediate value, when specialization constant 4 is not 0

#version 460

#include "lib/kernel.lib.glsl"

layout(constant_id = 3) const uint kArithOp = 0;
layout(constant_id = 4) const uint kImmediateOperand = 0;

#define ARITH_IMMEDIATE_NONE  0u
#define ARITH_IMMEDIATE_LEFT  1u
#define ARITH_IMMEDIATE_RIGHT 2u

#define ARITH_ADD      0u
#define ARITH_SUBTRACT 1u
#define ARITH_MULTIPLY 2u
#define ARITH_DIVIDE   3u
#define ARITH_MINIMUM  4u
#define ARITH_MAXIMUM  5u

float Apply(float a, float b) {
    switch (kArithOp) {
        case ARITH_ADD: return a + b;
        case ARITH_SUBTRACT: return a - b;
        case ARITH_MULTIPLY: return a * b;
        // A zero divisor yields zero rather than an infinity, so one bad sample cannot poison
        // everything downstream.
        case ARITH_DIVIDE: return b == 0.0 ? 0.0 : a / b;
        case ARITH_MINIMUM: return min(a, b);
        default: return max(a, b);
    }
}

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }

    uint index = SampleIndex(local);
    uint countA = max(ParamUint(0), 1u);
    uint countB = max(ParamUint(1), 1u);
    uint countOut = max(ParamUint(2), 1u);
    float immediate = ParamFloat(3);

    // The folded side has no buffer, so only the other slot is bound. Reading the unbound one would
    // dereference a null device address, which is why the buffer reference itself is conditional.
    FloatBuffer source = FloatBuffer(pc.inputs[kImmediateOperand == ARITH_IMMEDIATE_LEFT ? 1 : 0]);
    FloatBuffer other =
        FloatBuffer(pc.inputs[kImmediateOperand == ARITH_IMMEDIATE_NONE ? 1 : 0]);
    FloatBuffer target = FloatBuffer(pc.outputs[0]);

    for (uint c = 0u; c < countOut; ++c) {
        float left;
        float right;
        if (kImmediateOperand == ARITH_IMMEDIATE_LEFT) {
            left  = immediate;
            // A one-component operand is read at its only index, which is the broadcast.
            right = source.values[index * countB + (countB == 1u ? 0u : c)];
        } else if (kImmediateOperand == ARITH_IMMEDIATE_RIGHT) {
            left  = source.values[index * countA + (countA == 1u ? 0u : c)];
            right = immediate;
        } else {
            left  = source.values[index * countA + (countA == 1u ? 0u : c)];
            right = other.values[index * countB + (countB == 1u ? 0u : c)];
        }
        target.values[index * countOut + c] = Apply(left, right);
    }
}
