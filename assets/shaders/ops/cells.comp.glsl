// Noises.Cells: Worley / Voronoi cells, World Machine's Voronoi device, one octave.
//
// Class: pointwise (halo 0).
// Mappings: channel 0 is R2 -> R1 (the value), channel 1 is R2 -> R3 (the cell): the nearest site's
//           position in metres along x and z, and a random value in [0, 1) that is constant over the
//           cell. Feeding (x, z) to another generator's `position` makes it constant per cell, which
//           is World Machine's Crystallize; the random value is its Cellular F1 (a flat plate per cell).
//
// The sites are one per lattice cell, at the cell's centre moved by `jitter` times a random offset of
// up to half a cell: 0 is a regular grid of squares (or hexagons on the hex lattice), 1 is the
// classic Worley distribution. The 5 x 5 search is exact for the four nearest at any jitter up to 1
// except in rare configurations, which is also what World Machine does.
//
// Parameters:
//   0  frequency    cells per metre, strictly positive
//   1  style        0 F1, 1 F2, 2 F3, 3 F4, 4 F2-F1, 5 F3-F2, 6 F3-F1, 7 F4-F3, 8 F4-F2, 9 F4-F1,
//                   10 the cell's random value
//   2  bits 0-7: metric, 0 Euclidean, 1 Manhattan, 2 Chebyshev (chessboard), 3 quasi-Euclidean;
//      bit 8: hexagonal lattice (rows half a cell apart, sqrt(3)/2 high) instead of square;
//      bit 9: warped, input 0 is a position field in metres instead of the sample's own position
//   3  jitter       in [0, 1]
//   4  amplitude    output scale of the value
//   5  offset       added to the value
//   6  resolution   metres per sample
//   7  seed salt    decorrelates nodes that share the job seed
//   8, 9 are the iteration words every dispatch carries, so they hold nothing here
//
// Distances are in cells, so F1 is about 0.5 at most on a regular grid and up to about 1 with jitter.

#version 460

#include "lib/kernel.lib.glsl"
#include "lib/hash.lib.glsl"

float Metric(vec2 d, uint metric) {
    vec2 a = abs(d);
    if (metric == 1u) {
        return a.x + a.y;
    }
    if (metric == 2u) {
        return max(a.x, a.y);
    }
    if (metric == 3u) {
        return max(a.x, a.y) + 0.41421356 * min(a.x, a.y);
    }
    return length(d);
}

void main() {
    uvec3 local = LocalSample();
    if (!InsidePaddedSection(local)) {
        return;
    }

    uint  index      = SampleIndex(local);
    ivec3 world      = WorldSample(local);
    float frequency  = ParamFloat(0);
    uint  style      = ParamUint(1);
    uint  metric     = ParamUint(2) & 0xFFu;
    float jitter     = ParamFloat(3);
    float resolution = ParamFloat(6);
    uint  seed       = pc.seed ^ (ParamUint(7) * ENGINE_HASH_PHI);
    bool  hex        = (ParamUint(2) & 0x100u) != 0u;

    vec2  position = (ParamUint(2) & 0x200u) != 0u ? LoadVec2(0u, InputIndex(0u, local))
                                        : vec2(float(world.x), float(world.z)) * resolution;
    vec2  p        = position * frequency;
    float rowStep  = hex ? 0.8660254 : 1.0;
    ivec2 base     = ivec2(int(floor(p.x)), int(floor(p.y / rowStep)));

    float d[4]     = float[4](1e9, 1e9, 1e9, 1e9);
    vec2  site     = vec2(0.0);
    uint  nearest  = 0u;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            ivec2 lattice = base + ivec2(i, j);
            uint  h       = HashLattice2(lattice, seed);
            vec2  random  = vec2(HashToUnit(h), HashToUnit(HashMix(h ^ ENGINE_HASH_PHI))) - 0.5;
            float shift   = hex && (lattice.y & 1) != 0 ? 0.5 : 0.0;
            vec2  point   = vec2(float(lattice.x) + 0.5 + shift + jitter * random.x,
                                 (float(lattice.y) + 0.5 + jitter * random.y) * rowStep);
            float dd      = Metric(p - point, metric);
            // Insertion into the four nearest, strict, so ties keep the first found on both sides.
            if (dd < d[0]) {
                d[3] = d[2]; d[2] = d[1]; d[1] = d[0]; d[0] = dd;
                site = point;
                nearest = h;
            } else if (dd < d[1]) {
                d[3] = d[2]; d[2] = d[1]; d[1] = dd;
            } else if (dd < d[2]) {
                d[3] = d[2]; d[2] = dd;
            } else if (dd < d[3]) {
                d[3] = dd;
            }
        }
    }

    float cell  = HashToUnit(HashMix(nearest ^ 0x85EBCA6Bu));
    float value = style <= 3u ? d[style]
                : style == 4u ? d[1] - d[0]
                : style == 5u ? d[2] - d[1]
                : style == 6u ? d[2] - d[0]
                : style == 7u ? d[3] - d[2]
                : style == 8u ? d[3] - d[1]
                : style == 9u ? d[3] - d[0]
                              : cell;
    if (WantsChannel(0u)) {
        StoreScalar(0u, index, value * ParamFloat(4) + ParamFloat(5));
    }
    if (WantsChannel(1u)) {
        StoreVec3(1u, index, vec3(site / frequency, cell));
    }
}
