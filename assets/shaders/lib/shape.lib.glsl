// Signed distances to 2D shapes, in metres and negative inside, each in its own local space: centred
// on the origin and unrotated. Shared by `Shape`, one shape a node, and `Lands`, a list of them in one
// node; the C++ references in kernels.cpp and sampler.cpp are the same expressions.
//
// `vec2.x` is world x and `vec2.y` is world z.

#ifndef ENGINE_LIB_SHAPE_GLSL
#define ENGINE_LIB_SHAPE_GLSL

float SegmentDistance(vec2 p, vec2 a, vec2 b) {
    vec2  pa            = p - a;
    vec2  ba            = b - a;
    float lengthSquared = dot(ba, ba);
    float h             = lengthSquared > 0.0 ? dot(pa, ba) / lengthSquared : 0.0;
    h                   = clamp(h, 0.0, 1.0);
    return length(pa - ba * h);
}

// Whether the ray from `p` towards +x crosses the segment, with the half-open rule on z, which counts a
// vertex shared by two segments exactly once.
bool SegmentCrosses(vec2 p, vec2 a, vec2 b) {
    if ((a.y > p.y) == (b.y > p.y)) {
        return false;
    }
    float xAt = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
    return p.x < xAt;
}

float CircleDistance(vec2 p, float r) { return length(p) - r; }

// The usual bound on the distance rather than the exact one; the exact ellipse distance is a quartic.
// It is exact on the axes and only approximate away from the edge.
float EllipseDistance(vec2 p, vec2 r) {
    float k0 = length(p / r);
    float k1 = length(p / (r * r));
    return k1 > 0.0 ? k0 * (k0 - 1.0) / k1 : -min(r.x, r.y);
}

float BoxDistance(vec2 p, vec2 half_, float rounding) {
    vec2  q      = abs(p) - (half_ - vec2(rounding));
    float inside = max(q.x, q.y);
    return length(max(q, vec2(0.0))) + (inside < 0.0 ? inside : 0.0) - rounding;
}

float BoxChebyshevDistance(vec2 p, vec2 half_) {
    vec2 q = abs(p) - half_;
    return max(q.x, q.y);
}

#endif
