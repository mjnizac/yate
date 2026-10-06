// Fractal Brownian motion, generic over its base function.
//
// The algorithm lives here exactly once and names no base function. Instantiations belong to the op
// shader, which is what keeps "add a base function" down to one function and one line.
//
//   fbm(p, octaves, amplitude, persistence, frequency, lacunarity) : R^n -> (R, R^n)
//
// GLSL has no function pointers, templates or generics, and `subroutine` is OpenGL-only. Macro
// instantiation is the C-style answer: zero runtime cost, fully inlined, and the derivative
// propagates exactly.

#ifndef ENGINE_LIB_FBM_GLSL
#define ENGINE_LIB_FBM_GLSL

#include "lib/hash.lib.glsl"

// Per-octave weights: the octave amplitude for the value, and amplitude * frequency for the
// gradient, which is the chain rule for sampling the base at `p * frequency`.
//
// Passing the dimension in as a weight builder avoids a swizzle macro, so nothing here has to know
// whether the gradient is `.yz` or `.yzw`.
vec3 FbmWeight2(float a, float af) { return vec3(a, af, af); }
vec4 FbmWeight3(float a, float af) { return vec4(a, af, af, af); }

// ENGINE_DEFINE_FBM(Name, PointT, RetT, WeightFn, Base)
//
//   Base: (PointT p, uint seed) -> RetT, the value in .x and its *exact* gradient in the rest.
//         A base that returned only a value would leave the gradient channel unavailable, and with
//         it the single-dispatch, halo-free path that Normals and SlopeMask rely on.
//   Name: (PointT p, uint octaves, float amplitude, float persistence, float frequency,
//          float lacunarity, uint seed) -> RetT
//
// `kFbmNormalize` must be a compile-time constant in scope, normally a specialization constant, so
// the branch folds away when the pipeline is created rather than costing a test per sample.
//
// The multiply grouping is deliberate: one weight vector per octave, then one amplitude multiply at
// the very end. Float multiplication is not associative, so regrouping changes the last bit, and
// the dataset goldens are compared bit-exactly.
#define ENGINE_DEFINE_FBM(Name, PointT, RetT, WeightFn, Base)                                       \
    RetT Name(PointT p, uint octaves, float amplitude, float persistence, float frequency,           \
              float lacunarity, uint seed) {                                                        \
        RetT  sum  = RetT(0.0);                                                                     \
        float a    = 1.0;                                                                           \
        float f    = frequency;                                                                     \
        float norm = 0.0;                                                                           \
        for (uint o = 0u; o < octaves; ++o) {                                                       \
            sum += Base(p * f, seed + o * ENGINE_HASH_PHI) * WeightFn(a, a * f);                    \
            norm += a;                                                                              \
            a *= persistence;                                                                       \
            f *= lacunarity;                                                                        \
        }                                                                                           \
        return kFbmNormalize != 0u ? amplitude * (sum / norm) : amplitude * sum;                     \
    }

#endif // ENGINE_LIB_FBM_GLSL
