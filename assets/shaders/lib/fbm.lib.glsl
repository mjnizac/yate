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
//          float lacunarity, uint seed, float power) -> RetT
//
// `kFbmNormalize` must be a compile-time constant in scope, normally a specialization constant, so
// the branch folds away when the pipeline is created rather than costing a test per sample.
//
// The multiply grouping is deliberate: one weight vector per octave, then one amplitude multiply at
// the very end. Float multiplication is not associative, so regrouping changes the last bit, and
// the dataset goldens are compared bit-exactly.
//
// `kFbmMultiscale` (also a constant in scope) is World Machine's multiscale type: each octave is
// weighted by `w`, mixed towards 1 by `1 - power`. ENGINE_MULTISCALE_SIGNAL multiplies the weight by
// the previous octave's signal mapped to [0, 1] (Musgrave's hybrid multifractal: detail where the
// signal is high), ENGINE_MULTISCALE_ELEVATION sets it from the sum so far (heterogeneous terrain:
// detail on high ground, smooth lowlands). The weight carries its own gradient, so the result's
// gradient stays exact; `power` is taken as locally constant. Off, the loop is the plain sum above.
#define ENGINE_MULTISCALE_NONE      0u
#define ENGINE_MULTISCALE_SIGNAL    1u
#define ENGINE_MULTISCALE_ELEVATION 2u

#define ENGINE_DEFINE_FBM(Name, PointT, RetT, WeightFn, Base)                                       \
    RetT Name(PointT p, uint octaves, float amplitude, float persistence, float frequency,           \
              float lacunarity, uint seed, float power) {                                           \
        RetT  sum  = RetT(0.0);                                                                     \
        RetT  w    = RetT(0.0);                                                                     \
        float a    = 1.0;                                                                           \
        float f    = frequency;                                                                     \
        float norm = 0.0;                                                                           \
        w.x        = 1.0;                                                                           \
        for (uint o = 0u; o < octaves; ++o) {                                                       \
            if (kFbmMultiscale == ENGINE_MULTISCALE_NONE) {                                         \
                sum += Base(p * f, seed + o * ENGINE_HASH_PHI) * WeightFn(a, a * f);                \
            } else {                                                                                \
                RetT n = Base(p * f, seed + o * ENGINE_HASH_PHI) * WeightFn(1.0, f);                \
                if (kFbmMultiscale == ENGINE_MULTISCALE_ELEVATION && o > 0u) {                      \
                    float e = 0.5 + 0.5 * sum.x / norm;                                             \
                    w       = e > 0.0 && e < 1.0 ? sum * (0.5 * power / norm) : RetT(0.0);          \
                    w.x     = 1.0 - power + power * clamp(e, 0.0, 1.0);                             \
                }                                                                                   \
                RetT c = n * w.x + w * n.x;                                                         \
                c.x    = n.x * w.x;                                                                 \
                sum += c * a;                                                                       \
                if (kFbmMultiscale == ENGINE_MULTISCALE_SIGNAL) {                                   \
                    float s  = 0.5 + 0.5 * n.x;                                                     \
                    float m  = 1.0 - power + power * clamp(s, 0.0, 1.0);                            \
                    RetT  dm = s > 0.0 && s < 1.0 ? n * (0.5 * power) : RetT(0.0);                  \
                    RetT  nw = w * m + dm * w.x;                                                    \
                    nw.x     = w.x * m;                                                             \
                    w        = nw;                                                                  \
                }                                                                                   \
            }                                                                                       \
            norm += a;                                                                              \
            a *= persistence;                                                                       \
            f *= lacunarity;                                                                        \
        }                                                                                           \
        return kFbmNormalize != 0u ? amplitude * (sum / norm) : amplitude * sum;                     \
    }

#endif // ENGINE_LIB_FBM_GLSL
