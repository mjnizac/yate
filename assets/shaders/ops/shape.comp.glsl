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

float SegmentDistance(vec2 p, vec2 a, vec2 b) {
    vec2  pa            = p - a;
    vec2  ba            = b - a;
    float lengthSquared = dot(ba, ba);
    float h             = lengthSquared > 0.0 ? dot(pa, ba) / lengthSquared : 0.0;
    h                   = clamp(h, 0.0, 1.0);
    return length(pa - ba * h);
}

bool SegmentCrosses(vec2 p, vec2 a, vec2 b) {
    if ((a.y > p.y) == (b.y > p.y)) {
        return false;
    }
    float xAt = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
    return p.x < xAt;
}

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
        StoreScalar(0u, index, length(p) - ParamFloat(0));
    } else if (kShapeKind == SHAPE_ELLIPSE) {
        vec2  r  = vec2(ParamFloat(0), ParamFloat(1));
        float k0 = length(p / r);
        float k1 = length(p / (r * r));
        // The usual bound on the distance rather than the exact one; the exact ellipse distance is a
        // quartic. It is exact on the axes and only approximate away from the edge.
        StoreScalar(0u, index, k1 > 0.0 ? k0 * (k0 - 1.0) / k1 : -min(r.x, r.y));
    } else if (kShapeKind == SHAPE_BOX) {
        float rounding = ParamFloat(2);
        vec2  q        = abs(p) - (vec2(ParamFloat(0), ParamFloat(1)) - vec2(rounding));
        float inside   = max(q.x, q.y);
        StoreScalar(0u, index,
                    length(max(q, vec2(0.0))) + (inside < 0.0 ? inside : 0.0) - rounding);
    } else if (kShapeKind == SHAPE_BOX_CHEBYSHEV) {
        vec2 q = abs(p) - vec2(ParamFloat(0), ParamFloat(1));
        StoreScalar(0u, index, max(q.x, q.y));
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
