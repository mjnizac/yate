// Integer lattice hashing.
//
// Every procedural op hashes integer lattice coordinates, never floats. That is what makes a
// region evaluated as one section and as N x N sections produce bit-identical interiors, and it
// keeps precision intact far from the world origin (spec section 10).

#ifndef ENGINE_LIB_HASH_GLSL
#define ENGINE_LIB_HASH_GLSL

// Fixed-point multiplier used to fold a coordinate into the hash state.
#define ENGINE_HASH_PHI 0x9E3779B9u

// Two rounds of xor-multiply mixing. Avalanches well enough that neighbouring lattice cells
// produce uncorrelated gradients, which is all a noise function needs.
uint HashMix(uint x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

uint HashLattice2(ivec2 cell, uint seed) {
    uint h = seed;
    h = HashMix(h ^ (uint(cell.x) * ENGINE_HASH_PHI));
    h = HashMix(h ^ (uint(cell.y) * 0x85EBCA6Bu));
    return h;
}

uint HashLattice3(ivec3 cell, uint seed) {
    uint h = seed;
    h = HashMix(h ^ (uint(cell.x) * ENGINE_HASH_PHI));
    h = HashMix(h ^ (uint(cell.y) * 0x85EBCA6Bu));
    h = HashMix(h ^ (uint(cell.z) * 0xC2B2AE35u));
    return h;
}

// Uniform float in [0, 1) from a hash.
float HashToUnit(uint h) { return float(h >> 8) * (1.0 / 16777216.0); }

#endif // ENGINE_LIB_HASH_GLSL
