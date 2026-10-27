// Water: a river or a lake of Water.Network carved in one pass.
//
// Class: pointwise (halo 0). Input 0 is the position in metres (R2 -> R2), input 1 an auxiliary field
// (R2 -> R1, its meaning set by the mode); input slot 3 is the grid buffer, bound by the evaluator, where
// the element lives as a block:
//
//   header, 20 values:  0 width  1 depth  2 origin x  3 origin z  4 cell  5 kind  6..15 the kind's numbers
//                       16 point count  17 where the cells start, from the header
//   points, 6 values each (river only): x, z, level, half-width, depth, bank skew (where across the
//   channel its deepest line runs, -1 to 1, positive to the left looking downstream)
//   cells, 4 values each, x fastest:
//       river  squared distance to the channel, its nearest segment, its level, the valley axis's level
//       lake   signed distance to the shore (negative inside), unused, unused, unused
//
// The cells are coarse, a valley's scale. Near the channel its distance, level and half-width are
// measured exactly on the points, from the cell's nearest segment and the `window` (15) segments either
// side of it, so the channel is as sharp as the course is fine whatever the cells.
//
// Channel 0 is a bound on the ground: an upper one (the envelope the ground is cut down to) in the cap
// modes, a lower one in the fill and rim modes. Channel 1 is the water surface, far below the ground where
// there is none. The same arithmetic, term for term, is the CPU version in `sampler.cpp`.
//
// Parameters:
//   0  block     offset of the element's header in the buffer
//   1  receiver  offset of the river this one joins, or 0xFFFFFFFF
//   2  mode      0 river cap, 1 river fill, 2 lake cap, 3 lake rim
//
// River header 6..15: depth, valley, steep (tan of the side), step (canyon benches), floor (glacial floor
// share), plain (floodplain width), terrace, levee, width at the mouth, window (segments searched):
//   kind 0 mountain, 1 canyon, 2 gorge, 3 glacial, 4 lowland, 5 braided
// Lake header 6..15: level, depth, size, wall, shelf, shelf depth, basin tan, basin reach, islands, rim:
//   kind 0 bowl, 1 trench, 2 shelf, 3 crater, 4 cirque, 5 fan (a delta: land, not water; rim mode only)

#version 460

#include "lib/kernel.lib.glsl"

float CatmullRom(float a, float b, float c, float d, float t) {
    return b + 0.5 * t * (c - a + t * (2.0 * a - 5.0 * b + 4.0 * c - d + t * (3.0 * (b - c) + d - a)));
}

float At(FloatBuffer data, uint block, uint offset) { return data.values[block + offset]; }

// Component `k` of a block's cells at `position`, Catmull-Rom, the edge held.
float Field(FloatBuffer data, uint block, vec2 position, uint k) {
    int   width  = int(At(data, block, 0u));
    int   depth  = int(At(data, block, 1u));
    vec2  origin = vec2(At(data, block, 2u), At(data, block, 3u));
    float cell   = At(data, block, 4u);
    vec2  g      = clamp((position - origin) / cell, vec2(0.0), vec2(float(width - 1), float(depth - 1)));
    ivec2 i      = ivec2(floor(g));
    vec2  t      = g - vec2(i);
    float rows[4];
    for (int j = -1; j <= 2; ++j) {
        int   z = clamp(i.y + j, 0, depth - 1);
        float v[4];
        for (int m = -1; m <= 2; ++m) {
            int x = clamp(i.x + m, 0, width - 1);
            v[m + 1] = data.values[block + uint(At(data, block, 17u)) + uint(z * width + x) * 4u + k];
        }
        rows[j + 1] = CatmullRom(v[0], v[1], v[2], v[3], t.x);
    }
    return CatmullRom(rows[0], rows[1], rows[2], rows[3], t.y);
}

// The exact distance from `p` to the course of a river block, searched over the segments around the one
// the cell under `p` holds as nearest, with the level and the half-width there.
float Course(FloatBuffer data, uint block, vec2 p, out float level, out float halfWidth, out float depth,
             out float skew, out float side) {
    int   width  = int(At(data, block, 0u));
    int   rows   = int(At(data, block, 1u));
    vec2  origin = vec2(At(data, block, 2u), At(data, block, 3u));
    float cell   = At(data, block, 4u);
    ivec2 c      = clamp(ivec2(floor((p - origin) / cell + 0.5)), ivec2(0), ivec2(width - 1, rows - 1));
    int   nearest = int(data.values[block + uint(At(data, block, 17u)) + uint(c.y * width + c.x) * 4u + 1u] + 0.5);
    int   window  = int(At(data, block, 15u));
    int   count   = int(At(data, block, 16u));
    float best    = 1e30;
    level     = 0.0;
    halfWidth = 0.5;
    depth     = 1.0;
    skew      = 0.0;
    side      = 1.0;
    for (int s = max(0, nearest - window); s <= min(count - 2, nearest + window); ++s) {
        uint  a  = block + 20u + uint(s) * 6u;
        vec2  pa = vec2(data.values[a], data.values[a + 1u]);
        vec2  pb = vec2(data.values[a + 6u], data.values[a + 7u]);
        vec2  ab = pb - pa;
        float t  = clamp(dot(p - pa, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
        float d  = length(p - pa - ab * t);
        if (d < best) {
            best      = d;
            level     = mix(data.values[a + 2u], data.values[a + 8u], t);
            halfWidth = mix(data.values[a + 3u], data.values[a + 9u], t);
            depth     = mix(data.values[a + 4u], data.values[a + 10u], t);
            skew      = mix(data.values[a + 5u], data.values[a + 11u], t);
            vec2 off  = p - pa - ab * t;
            side      = ab.x * off.y - ab.y * off.x >= 0.0 ? 1.0 : -1.0;
        }
    }
    return best;
}

float Terrace(float value, float step, float sharpness) {
    float t     = value / step;
    float whole = floor(t);
    float s     = 1.0 - sharpness;
    float r     = s < 1e-4 ? (t - whole >= 0.5 ? 1.0 : 0.0) : smoothstep(0.5 - 0.5 * s, 0.5 + 0.5 * s, t - whole);
    return step * (whole + r);
}

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }
    uint        index    = SampleIndex(local);
    vec2        p        = LoadVec2(0u, InputIndex(0u, local));
    float       aux      = LoadScalar(1u, InputIndex(1u, local));
    FloatBuffer data     = FloatBuffer(pc.inputs[3]);
    uint        block    = ParamUint(0);
    uint        receiver = ParamUint(1);
    uint        mode     = ParamUint(2);
    float       bound    = 0.0;
    float       water    = -1e6;

    if (mode <= 1u) {
        float exactLevel;
        float exactHalf;
        float depth;
        float skew;
        float side;
        float exact  = Course(data, block, p, exactLevel, exactHalf, depth, skew, side);
        float d      = min(exact, sqrt(max(Field(data, block, p, 0u), 0.0)));
        float cellW  = At(data, block, 4u);
        float level  = mix(Field(data, block, p, 2u), exactLevel, 1.0 - smoothstep(cellW, 2.0 * cellW, exact));
        float half_  = max(exactHalf, 0.5);
        float axis   = Field(data, block, p, 3u);
        uint  kind   = uint(At(data, block, 5u));
        float valley = At(data, block, 7u);
        float steep  = At(data, block, 8u);
        float plain  = At(data, block, 11u);
        float e      = d - half_;
        if (mode == 0u) {
            // Across the channel, -1 to 1: the deepest line runs at `skew`, against the outer bank of a bend
            // (a cut bank), and the bed rises gently from it to the inner one (a point bar).
            float u   = d / half_;
            float v   = side * u;
            float t   = v > skew ? (v - skew) / (1.0 - skew) : (v - skew) / (1.0 + skew);
            float cut = clamp(1.0 - t * t, 0.0, 1.0);
            if (kind == 5u) {
                // Several shallow threads across a wide bed, gravel bars between them at the water.
                cut *= 0.5 + 0.5 * cos(u * 9.4248 + 3.0 * sin(dot(p, vec2(0.0041, 0.0033))));
            }
            float bed  = level - cut * depth;
            float away = smoothstep(At(data, block, 14u) * 1.5, At(data, block, 14u) * 6.0, e);
            bed += (axis - level) * away;
            float open  = max(e, 0.0);
            float flare = open;
            float rise;
            if (kind == 1u) {
                rise = Terrace(open * steep, At(data, block, 9u), 0.92);
            } else if (kind == 3u) {
                float floor_ = valley * At(data, block, 10u);
                float past   = max(e - floor_, 0.0);
                rise  = past * past * (steep / (2.0 * (valley - floor_)));
                flare = past;
            } else if (kind == 4u || kind == 5u) {
                float terrace = At(data, block, 12u);
                rise = smoothstep(0.0, depth * 2.0, e) + smoothstep(plain, plain + 60.0, e) * terrace
                     + smoothstep(plain * 1.5, plain * 1.5 + 60.0, e) * terrace
                     + max(e - plain * 1.5 - 60.0, 0.0) * steep;
            } else {
                rise = open * steep;
            }
            rise += flare * flare * (steep / valley);
            bound = bed + rise * aux;
            water = level - smoothstep(0.0, 3.0, e) * 20000.0;
            if (receiver != 0xFFFFFFFFu) {
                // Inside the river it joins, the water is that river's: this one gives way there, and only
                // there, so running close beside it upstream it keeps its own.
                float rl, rh, rd, rs, rside;
                float er = Course(data, receiver, p, rl, rh, rd, rs, rside) - max(rh, 0.5);
                water -= (1.0 - smoothstep(-2.0, 0.0, er)) * 20000.0;
            }
        } else {
            float levee  = (smoothstep(0.0, depth * 3.0, e) - smoothstep(depth * 3.0, depth * 12.0, e)) * At(data, block, 13u);
            float within = smoothstep(0.0, depth, e) - smoothstep(plain, plain + 30.0, e);
            float fill   = min(level + 0.6 + levee, aux + 3.0);
            bound = fill * within + (level - 20000.0) * (1.0 - within);
        }
    } else {
        float s       = Field(data, block, p, 0u);
        float q       = -s;
        float L       = At(data, block, 6u);
        float D       = At(data, block, 7u);
        float size    = At(data, block, 8u);
        float cell    = At(data, block, 4u);
        float outside = smoothstep(-cell, 0.0, s);
        if (mode == 2u) {
            float shelf = size * At(data, block, 10u);
            float floor_;
            if (shelf > 0.0) {
                float sd = At(data, block, 11u);
                floor_ = L - smoothstep(0.0, shelf * 0.3, q) * (D * sd)
                       - smoothstep(shelf, shelf + size * At(data, block, 9u), q) * (D * (1.0 - sd));
            } else {
                floor_ = L - smoothstep(0.0, size * At(data, block, 9u), q) * D;
            }
            // Relief under the water, and islands where it rises above it.
            float peak  = aux + 0.5;
            float under = smoothstep(size * 0.05, size * 0.3, q);
            floor_ += peak * peak * under * (D * (0.3 + At(data, block, 14u) * 2.2));
            float around = L + 2.0 + max(s, 0.0) * At(data, block, 12u) + max(s - At(data, block, 13u), 0.0) * 20.0;
            bound = floor_ * (1.0 - outside) + around * outside;
            water = L - smoothstep(30.0, 60.0, s) * 20000.0;
        } else {
            float rim = (L + 2.0) - smoothstep(0.0, 120.0, s) * 20000.0;
            bound = rim * outside + (L - 20000.0) * (1.0 - outside);
            if (uint(At(data, block, 5u)) == 5u) {
                // A delta's fan: land a metre and a half over the water inside the outline, shelving under it over
                // its last 150 m; the distributaries cut through it afterwards.
                float land = L + 1.5 - (1.0 - smoothstep(0.0, 150.0, q)) * 5.0;
                bound = land * (1.0 - outside) + (L - 20000.0) * outside;
            }
            float crater = At(data, block, 15u);
            if (crater > 0.0) {
                float x     = abs(s - size * 0.25) / (size * 0.12);
                float crest = exp(-0.5 * x * x);
                bound = max(bound, (L + crater) * crest + (L - 20000.0) * (1.0 - crest));
            }
        }
    }
    if (WantsChannel(0)) {
        StoreScalar(0u, index, bound);
    }
    if (WantsChannel(1)) {
        StoreScalar(1u, index, water);
    }
}
