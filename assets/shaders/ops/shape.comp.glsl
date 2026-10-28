// Signed distance, in metres and negative inside, to a shape evaluated at a position field.
//
// Every shape is in its own local space, centred on the origin and unrotated; a `Transform` on the
// position places it. Pointwise, so it needs no halo and two sections agree on a shared border by
// construction.
//
// `vec2.x` is world x and `vec2.y` is world z, as everywhere else in the engine.
//
// Polygons and polylines are chains of segment nodes, because a kernel has ten parameter words and no
// buffers. Each segment node folds one or two segments into an accumulator: the distance so far in x
// and the parity of the crossings in y. A resolve node turns the finished accumulator into a signed
// distance. The parity is a ray cast towards +x with the half-open rule on z, which counts a vertex
// shared by two segments exactly once; `EvalShape` in kernels.cpp is the same expression.
//
// Variant fields: 0 is the shape, 1 is the number of segments in this node.
//
// Parameters: circle: r. Ellipse: rx, rz. Box: half x, half z, rounding. BoxChebyshev: half x, half z.
// Segments: words 0..3 are the first segment (ax, az, bx, bz) and 4..7 the second.

#version 460

#include "lib/kernel.lib.glsl"
#include "lib/shape.lib.glsl"

layout(constant_id = 3) const uint kShapeKind     = 0;
layout(constant_id = 4) const uint kShapeSegments = 1;

#define SHAPE_CIRCLE           0u
#define SHAPE_ELLIPSE          1u
#define SHAPE_BOX              2u
#define SHAPE_BOX_CHEBYSHEV    3u
#define SHAPE_SEGMENTS_FIRST   4u
#define SHAPE_SEGMENTS_NEXT    5u
#define SHAPE_RESOLVE_POLYGON  6u
#define SHAPE_RESOLVE_POLYLINE 7u

const float kNoDistance = 1.0e30;

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }

    uint index = SampleIndex(local);

    if (kShapeKind == SHAPE_RESOLVE_POLYGON || kShapeKind == SHAPE_RESOLVE_POLYLINE) {
        vec2  accumulator = LoadVec2(0u, InputIndex(0u, local));
        float distance_   = accumulator.x;
        if (kShapeKind == SHAPE_RESOLVE_POLYGON && accumulator.y > 0.5) {
            distance_ = -distance_;
        }
        StoreScalar(0u, index, distance_);
        return;
    }

    vec2 p = LoadVec2(0u, InputIndex(0u, local));

    if (kShapeKind == SHAPE_CIRCLE) {
        StoreScalar(0u, index, CircleDistance(p, ParamFloat(0)));
    } else if (kShapeKind == SHAPE_ELLIPSE) {
        StoreScalar(0u, index, EllipseDistance(p, vec2(ParamFloat(0), ParamFloat(1))));
    } else if (kShapeKind == SHAPE_BOX) {
        StoreScalar(0u, index, BoxDistance(p, vec2(ParamFloat(0), ParamFloat(1)), ParamFloat(2)));
    } else if (kShapeKind == SHAPE_BOX_CHEBYSHEV) {
        StoreScalar(0u, index, BoxChebyshevDistance(p, vec2(ParamFloat(0), ParamFloat(1))));
    } else {
        float distance_ = kNoDistance;
        float parity    = 0.0;
        if (kShapeKind == SHAPE_SEGMENTS_NEXT) {
            vec2 accumulator = LoadVec2(1u, InputIndex(1u, local));
            distance_        = accumulator.x;
            parity           = accumulator.y;
        }
        for (uint s = 0u; s < kShapeSegments; ++s) {
            uint at = s * 4u;
            vec2 a  = vec2(ParamFloat(at), ParamFloat(at + 1u));
            vec2 b  = vec2(ParamFloat(at + 2u), ParamFloat(at + 3u));
            distance_ = min(distance_, SegmentDistance(p, a, b));
            if (SegmentCrosses(p, a, b)) {
                parity = 1.0 - parity;
            }
        }
        StoreVec2(0u, index, vec2(distance_, parity));
    }
}
