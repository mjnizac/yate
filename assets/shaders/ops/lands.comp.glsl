// Lands: a list of shapes, combined in order, in one node.
//
// Class: pointwise (halo 0). Input 0 is the position in metres (R2 -> R2); input slot 3 is the grid
// buffer, bound by the evaluator, where the list lives as a block. Channel 0 is the signed distance to
// the coast in metres (land negative), held to [-reach, reach]; channel 1 is the id of the land element
// nearest the sample, 0 where there is none.
//
// Every element is a shape in its own local space (centred, unrotated, unscaled; see shape.lib.glsl),
// placed by a centre, a rotation and a scale. Land elements are joined to what came before them with a
// smooth minimum of radius `k` metres, water elements cut it with a smooth maximum: the order of the
// list is the order of the operations. The C++ version, term for term, is `EvalLands` in sampler.cpp.
//
// The elements are found through a regular grid of bins, each holding the indices of the elements that
// reach it, in list order; outside the bins only the global elements (those that cover everything)
// apply. The cost of a sample is the elements of its bin, whatever the length of the list.
//
// Block, offsets relative to its start:
//   header, 24 values:  0 element count  1 bin size  2..3 bin origin x, z  4..5 bins in x, z  6 reach
//                       7 elements  8 bin offsets (bins + 1 of them, into the lists)  9 lists
//                       10 points  11 global count  12 global list  13..23 unused
//   elements, 48 values each:
//       0 kind  1 operation (0 land, 1 water, 2 seabed)  2 id  3..4 centre x, z  5..6 cos, sin of the rotation
//       7 scale  8..19 the shape's numbers  20 smoothing k (m)  21 bounding radius (m)
//       22 first point  23 point count  24..35 the profile (relief mode)  36 inscribed radius (ellipse,
//       superellipse, radial)  37..39 a land element's trench: depth, offset from its coast, sigma  40..47 unused
//   points, 3 values each (x, z, width), in the element's local space.
//
// Shapes and their numbers (8..):
//   0 circle r  1 ellipse rx, rz  2 box half x, half z, rounding  3 superellipse a, b, n
//   4 radial R, harmonics m, then (k, a, phase) m times, at most 3  5 capsule half length, r
//   6 ring R, width  7 arc R, half aperture (rad), width  8 sector reach, half angle (rad)
//   9 polygon (points)  10 band (points with their widths)  11 fill (everywhere)
//
// Profiles (24.., relief mode), over s, the distance inland from the coast of the whole set:
//   0 none  1 dome H, W  2 cone H, W  3 plateau H, edge  4 rim: a crest of height Hr at c metres inland
//   (gaussian, sigma), plus an interior rising to Hi over W metres from `start`: 24 kind, 25 Hr, 26 c,
//   27 sigma, 28 Hi, 29 W, 30 start  5 tilt H0, edge, gradient, direction x, z, floor: a plane rising across
//   the element from its centre, H0 at the centre, held smoothly above `floor`
// In relief mode channel 0 is the height above the sea: each land element containing the sample raises its
// profile over its own distance inland held to the set's, and the profiles join by a smooth maximum
// `blend` metres wide (header 13).
//
// In seabed mode channel 0 is what to add to the sea floor: seabed elements (operation 2, which the coast
// ignores) raise their profiles over their own distance inland, and land elements with a trench sink it.
//
// Parameters: 0 block offset, 1 mode (0 coast, 1 relief, 2 seabed).

#version 460

#include "lib/kernel.lib.glsl"
#include "lib/shape.lib.glsl"

#define LANDS_HEADER  24u
#define LANDS_ELEMENT 48u

float At(FloatBuffer data, uint i) { return data.values[i]; }

float SmoothMin(float a, float b, float k) {
    if (k <= 0.0) {
        return min(a, b);
    }
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

float SuperellipseDistance(vec2 p, float a, float b, float n) {
    float u = max(abs(p.x) / a, 1e-20);
    float v = max(abs(p.y) / b, 1e-20);
    float s = pow(u, n) + pow(v, n);
    if (s < 1e-12) {
        return -min(a, b);
    }
    float r      = pow(s, 1.0 / n);
    float factor = pow(s, 1.0 / n - 1.0);
    vec2  g      = vec2(factor * pow(u, n - 1.0) / a, factor * pow(v, n - 1.0) / b);
    return (r - 1.0) / max(length(g), 1e-9);
}

float RadialDistance(FloatBuffer data, uint e, vec2 p) {
    float radius = At(data, e + 8u);
    uint  count  = uint(At(data, e + 9u));
    float rho    = length(p);
    float theta  = atan(p.y, p.x);
    float r      = 1.0;
    float dr     = 0.0;
    for (uint i = 0u; i < count; ++i) {
        float k     = At(data, e + 10u + i * 3u);
        float a     = At(data, e + 11u + i * 3u);
        float phase = At(data, e + 12u + i * 3u);
        r  += a * sin(k * theta + phase);
        dr += a * k * cos(k * theta + phase);
    }
    r  *= radius;
    dr *= radius;
    float g = dr / max(rho, At(data, e + 36u));
    return (rho - r) / sqrt(1.0 + g * g);
}

float ArcDistance(vec2 p, float radius, float aperture, float width) {
    vec2 q  = vec2(abs(p.y), p.x);
    vec2 sc = vec2(sin(aperture), cos(aperture));
    float d = sc.y * q.x > sc.x * q.y ? length(q - sc * radius) : abs(length(q) - radius);
    return d - 0.5 * width;
}

float SectorDistance(vec2 p, float reach, float angle) {
    vec2  q = vec2(abs(p.y), p.x);
    vec2  c = vec2(sin(angle), cos(angle));
    float l = length(q) - reach;
    float m = length(q - c * clamp(dot(q, c), 0.0, reach));
    return max(l, m * sign(c.y * q.x - c.x * q.y));
}

float PointsDistance(FloatBuffer data, uint points, uint first, uint count, vec2 p, bool polygon) {
    float best   = 1e30;
    bool  inside = false;
    for (uint i = 0u; i + (polygon ? 0u : 1u) < count; ++i) {
        uint  a  = points + (first + i) * 3u;
        uint  b  = points + (first + (i + 1u) % count) * 3u;
        vec2  pa = vec2(At(data, a), At(data, a + 1u));
        vec2  pb = vec2(At(data, b), At(data, b + 1u));
        if (polygon) {
            best = min(best, SegmentDistance(p, pa, pb));
            if (SegmentCrosses(p, pa, pb)) {
                inside = !inside;
            }
        } else {
            vec2  ab = pb - pa;
            float h  = clamp(dot(p - pa, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
            float w  = mix(At(data, a + 2u), At(data, b + 2u), h);
            best     = min(best, length(p - pa - ab * h) - 0.5 * w);
        }
    }
    return polygon && inside ? -best : best;
}

// The signed distance to element `e` (its absolute offset) at world position `world`.
float ElementDistance(FloatBuffer data, uint e, uint points, vec2 world, float reach) {
    uint  kind  = uint(At(data, e));
    vec2  d     = world - vec2(At(data, e + 3u), At(data, e + 4u));
    float c     = At(data, e + 5u);
    float s     = At(data, e + 6u);
    float scale = At(data, e + 7u);
    vec2  p     = vec2(c * d.x + s * d.y, -s * d.x + c * d.y) / scale;
    float a     = At(data, e + 8u);
    float b     = At(data, e + 9u);
    float n     = At(data, e + 10u);
    float dist;
    switch (kind) {
        case 0u: dist = CircleDistance(p, a); break;
        case 1u: dist = EllipseDistance(p, vec2(a, b)); break;
        case 2u: dist = BoxDistance(p, vec2(a, b), n); break;
        case 3u: dist = SuperellipseDistance(p, a, b, n); break;
        case 4u: dist = RadialDistance(data, e, p); break;
        case 5u: dist = SegmentDistance(p, vec2(-a, 0.0), vec2(a, 0.0)) - b; break;
        case 6u: dist = abs(length(p) - a) - 0.5 * b; break;
        case 7u: dist = ArcDistance(p, a, b, n); break;
        case 8u: dist = SectorDistance(p, a, b); break;
        case 9u:
        case 10u:
            dist = PointsDistance(data, points, uint(At(data, e + 22u)), uint(At(data, e + 23u)), p, kind == 9u);
            break;
        default: return -reach;
    }
    // Inside the inscribed circle the distance of the oval and radial shapes is that circle's, blended in: their
    // estimates depend on the direction there and would make a star at the centre. The edge is untouched.
    if (kind == 1u || kind == 3u || kind == 4u) {
        float inner = At(data, e + 36u);
        dist        = mix(length(p) - inner, dist, smoothstep(0.25 * inner, inner, length(p)));
    }
    return dist * scale;
}

float Profile(FloatBuffer data, uint e, float s, vec2 world) {
    uint  kind = uint(At(data, e + 24u));
    float a    = At(data, e + 25u);
    float b    = At(data, e + 26u);
    switch (kind) {
        case 1u: {
            float w = 1.0 - clamp(s / b, 0.0, 1.0);
            return a * sqrt(max(1.0 - w * w, 0.0));
        }
        case 2u: return a * clamp(s / b, 0.0, 1.0);
        case 3u: return a * smoothstep(0.0, b, s);
        case 4u: {
            float t = (s - b) / At(data, e + 27u);
            return a * exp(-0.5 * t * t) * smoothstep(0.0, b, s)
                 + At(data, e + 28u) * smoothstep(At(data, e + 30u), At(data, e + 30u) + At(data, e + 29u), s);
        }
        case 5u: {
            vec2  u     = vec2(At(data, e + 28u), At(data, e + 29u));
            float along = dot(world - vec2(At(data, e + 3u), At(data, e + 4u)), u);
            // The plane, held above its floor by a smooth maximum so the low side is level ground, not a crease.
            float floor_ = At(data, e + 30u);
            return smoothstep(0.0, b, s) * -SmoothMin(-(a + At(data, e + 27u) * along), -floor_, max(0.25 * abs(a - floor_), 1.0));
        }
        default: return 0.0;
    }
}

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }
    uint        index = SampleIndex(local);
    vec2        p     = LoadVec2(0u, InputIndex(0u, local));
    FloatBuffer data  = FloatBuffer(pc.inputs[3]);
    uint        block = ParamUint(0);

    float bin      = At(data, block + 1u);
    vec2  origin   = vec2(At(data, block + 2u), At(data, block + 3u));
    int   binsX    = int(At(data, block + 4u));
    int   binsZ    = int(At(data, block + 5u));
    float reach    = At(data, block + 6u);
    uint  elements = block + uint(At(data, block + 7u));
    uint  offsets  = block + uint(At(data, block + 8u));
    uint  lists    = block + uint(At(data, block + 9u));
    uint  points   = block + uint(At(data, block + 10u));

    uint  first = block + uint(At(data, block + 12u));
    uint  count = uint(At(data, block + 11u));
    ivec2 g     = ivec2(floor((p - origin) / bin));
    if (g.x >= 0 && g.y >= 0 && g.x < binsX && g.y < binsZ) {
        uint b = uint(g.y * binsX + g.x);
        uint s = uint(At(data, offsets + b));
        first  = lists + s;
        count  = uint(At(data, offsets + b + 1u)) - s;
    }

    float d    = reach;
    float best = 1e30;
    float id   = 0.0;
    for (uint i = 0u; i < count; ++i) {
        uint  e    = elements + uint(At(data, first + i)) * LANDS_ELEMENT;
        float op   = At(data, e + 1u);
        if (op > 1.5) {
            continue; // A seabed element raises the sea floor only (seabed mode).
        }
        float dist = ElementDistance(data, e, points, p, reach);
        float k    = At(data, e + 20u);
        if (op < 0.5) {
            d = SmoothMin(d, dist, k);
            if (dist < best) {
                best = dist;
                id   = At(data, e + 2u);
            }
        } else {
            d = -SmoothMin(-d, dist, k);
            // Land before a water element that covers the sample no longer owns it: an islet in a bay is
            // its own, not the island the bay was cut from.
            if (dist < 0.0) {
                best = 1e30;
            }
        }
        // Held at every step, so an element whose bounding circle is `reach + k` away from a sample
        // changes nothing there and the bins may leave it out.
        d = clamp(d, -reach, reach);
    }
    float value = d;
    if (ParamUint(1) == 1u) {
        // Each land element containing the sample raises its profile over its own distance inland, held
        // to the set's (so a bay or a lake cut later brings it down to the shore); the profiles join by a
        // smooth maximum `blend` metres wide. An element that does not contain the sample adds nothing,
        // which is also why the bins may leave it out.
        float blend = max(At(data, block + 13u), 1e-3);
        float shore = max(-d, 0.0);
        bool  any   = false;
        for (uint i = 0u; i < count; ++i) {
            uint e = elements + uint(At(data, first + i)) * LANDS_ELEMENT;
            if (At(data, e + 1u) < 0.5) {
                float s = min(-ElementDistance(data, e, points, p, reach), shore);
                if (s > 0.0) {
                    float h = Profile(data, e, s, p);
                    value   = any ? -SmoothMin(-value, -h, blend) : h;
                    any     = true;
                }
            }
        }
        value = any ? value : 0.0;
    }
    if (ParamUint(1) == 2u) {
        // The sea floor's own forms: seabed elements raise their profiles over their own distance inland
        // (joined by a smooth maximum), and a land element with a trench sinks a gaussian of its depth
        // `offset` metres off its coast. Channel 0 is what to add to the margin's floor.
        float blend  = max(At(data, block + 13u), 1e-3);
        float raise  = 0.0;
        float trench = 0.0;
        bool  any    = false;
        for (uint i = 0u; i < count; ++i) {
            uint  e  = elements + uint(At(data, first + i)) * LANDS_ELEMENT;
            float op = At(data, e + 1u);
            if (op > 1.5) {
                float s = -ElementDistance(data, e, points, p, reach);
                if (s > 0.0) {
                    float h = Profile(data, e, s, p);
                    raise   = any ? -SmoothMin(-raise, -h, blend) : h;
                    any     = true;
                }
            } else if (op < 0.5 && At(data, e + 37u) > 0.0) {
                float t = (ElementDistance(data, e, points, p, reach) - At(data, e + 38u)) / At(data, e + 39u);
                trench  = max(trench, At(data, e + 37u) * exp(-0.5 * t * t));
            }
        }
        value = raise - trench;
    }
    if (WantsChannel(0u)) {
        StoreScalar(0u, index, value);
    }
    if (WantsChannel(1u)) {
        StoreScalar(1u, index, id);
    }
}
