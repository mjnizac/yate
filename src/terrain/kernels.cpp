#include <engine/terrain/kernels.hpp>

#include <engine/assert.hpp>
#include <engine/log.hpp>
#include <engine/vulkan/context.hpp>

#ifdef TRACY_ENABLE
#    include <tracy/Tracy.hpp>
#endif

#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

namespace engine::terrain {

namespace {

using Clock        = std::chrono::steady_clock;
using Milliseconds = std::chrono::duration<f64_t, std::milli>;

/// The op registry. One entry and one shader is all an op needs.
constexpr OpInfo kOps[] = {
    {.kind           = OpKind::Const,
     .name           = "Const",
     .shader         = "ops/constant.comp.spv",
     .nodeClass      = NodeClass::Pointwise,
     .maxInputs      = 0,
     .maxChannels    = 1,
     .resolutionWord = kMaxNodeParams,
     .hasReference   = true,
     .specFieldCount = 0},
    {.kind           = OpKind::Coords,
     .name           = "Coords",
     .shader         = "ops/coords.comp.spv",
     .nodeClass      = NodeClass::Pointwise,
     .maxInputs      = 0,
     .maxChannels    = 1,
     .resolutionWord = kMaxNodeParams,
     .hasReference   = false, // Depends on world position, so it is not foldable.
     .specFieldCount = 0},
    {.kind           = OpKind::Noise,
     .name           = "Noise",
     .shader         = "ops/fbm.comp.spv",
     .nodeClass      = NodeClass::Pointwise,
     .maxInputs      = 0,
     .maxChannels    = 2,
     .resolutionWord = 6,
     .hasReference   = false, // Has its own reference, `EvalNoise`, not the pointwise one.
     // Two axes: the base function and whether the octave sum is normalized.
     .specFieldCount = 2},
    {.kind           = OpKind::Normals,
     .name           = "Normals",
     .shader         = "ops/normals.comp.spv",
     .nodeClass      = NodeClass::Pointwise,
     .maxInputs      = 1,
     .maxChannels    = 1,
     .resolutionWord = kMaxNodeParams,
     .hasReference   = true,
     .specFieldCount = 0},
    {.kind           = OpKind::Arith,
     .name           = "Arith",
     .shader         = "ops/arith.comp.spv",
     .nodeClass      = NodeClass::Pointwise,
     .maxInputs      = 2,
     .maxChannels    = 1,
     .resolutionWord = kMaxNodeParams,
     .hasReference   = true,
     // Field 0 is the operation, field 1 says which operand the compiler folded into an immediate.
     .specFieldCount = 2},
    {.kind           = OpKind::Curve,
     .name           = "Curve",
     .shader         = "ops/curve.comp.spv",
     .nodeClass      = NodeClass::Pointwise,
     .maxInputs      = 1,
     .maxChannels    = 1,
     .resolutionWord = kMaxNodeParams,
     .hasReference   = true,
     .specFieldCount = 1},
    {.kind           = OpKind::Blend,
     .name           = "Blend",
     .shader         = "ops/blend.comp.spv",
     .nodeClass      = NodeClass::Pointwise,
     .maxInputs      = 3,
     .maxChannels    = 1,
     .resolutionWord = kMaxNodeParams,
     .hasReference   = true,
     .specFieldCount = 0},
    {.kind           = OpKind::SlopeMask,
     .name           = "SlopeMask",
     .shader         = "ops/slope_mask.comp.spv",
     .nodeClass      = NodeClass::Pointwise,
     .maxInputs      = 1,
     .maxChannels    = 1,
     .resolutionWord = kMaxNodeParams,
     .hasReference   = true,
     .specFieldCount = 0},
    {.kind           = OpKind::Vector,
     .name           = "Vector",
     .shader         = "ops/vector.comp.spv",
     .nodeClass      = NodeClass::Pointwise,
     .maxInputs      = 4,
     .maxChannels    = 1,
     .resolutionWord = kMaxNodeParams,
     .hasReference   = true,
     .specFieldCount = 1},
    {.kind           = OpKind::Blur,
     .name           = "Blur",
     .shader         = "ops/blur.comp.spv",
     .nodeClass      = NodeClass::Neighborhood,
     .maxInputs      = 1,
     .maxChannels    = 1,
     .resolutionWord = kMaxNodeParams,
     // Reads a neighbourhood, so the pointwise reference cannot evaluate it; test_kernels compares
     // it against a CPU box blur of its own.
     .hasReference   = false,
     .specFieldCount = 0},
    {.kind           = OpKind::ThermalErosion,
     .name           = "ThermalErosion",
     .shader         = "ops/thermal_erosion.comp.spv",
     .nodeClass      = NodeClass::Iterative,
     .maxInputs      = 1,
     .maxChannels    = 1,
     .resolutionWord = kMaxNodeParams,
     // No CPU reference: it is iterative, so folding it would mean running the whole scheme on the
     // host, and it can never have constant inputs in practice anyway.
     .hasReference   = false,
     .specFieldCount = 0},
};
static_assert(ArrayCount(kOps) == static_cast<usize_t>(OpKind::Count));

[[nodiscard]] f32_t UnpackFloat(const std::array<u32_t, kMaxNodeParams>& params, usize_t word) {
    f32_t value = 0.0f;
    std::memcpy(&value, &params[word], sizeof(f32_t));
    return value;
}

// --- CPU reference: pointwise ops ----------------------------------------------------------------

[[nodiscard]] f32_t ApplyArith(ArithOp op, f32_t a, f32_t b) {
    switch (op) {
        case ArithOp::Add: return a + b;
        case ArithOp::Subtract: return a - b;
        case ArithOp::Multiply: return a * b;
        case ArithOp::Divide: return b == 0.0f ? 0.0f : a / b;
        case ArithOp::Minimum: return a < b ? a : b;
        case ArithOp::Maximum: return a > b ? a : b;
    }
    return 0.0f;
}

[[nodiscard]] f32_t Smoothstep(f32_t edge0, f32_t edge1, f32_t value) {
    if (edge0 == edge1) {
        return value < edge0 ? 0.0f : 1.0f;
    }
    f32_t t = (value - edge0) / (edge1 - edge0);
    t       = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

[[nodiscard]] f32_t ApplyCurve(CurveOp op, f32_t value, f32_t a, f32_t b) {
    switch (op) {
        case CurveOp::Clamp: return value < a ? a : (value > b ? b : value);
        case CurveOp::Remap: return a + (b - a) * (value * 0.5f + 0.5f);
        case CurveOp::Power: {
            const f32_t exponent = a > 0.0001f ? a : 0.0001f;
            const f32_t magnitude = std::pow(std::fabs(value), exponent);
            return value < 0.0f ? -magnitude : (value > 0.0f ? magnitude : 0.0f);
        }
        case CurveOp::Smoothstep: return Smoothstep(a, b, value);
    }
    return value;
}

// --- CPU reference: simplex noise ----------------------------------------------------------------
//
// A deliberate second implementation of what assets/shaders/lib/noise.glsl does. Having the two
// agree to within float rounding is what `test_kernels` checks, and it is the only way a change to
// one of them cannot silently diverge from the other.

[[nodiscard]] u32_t HashMix(u32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

constexpr u32_t kHashPhi = 0x9E3779B9u;

[[nodiscard]] u32_t HashLattice2(i32_t x, i32_t y, u32_t seed) {
    u32_t h = seed;
    h       = HashMix(h ^ (static_cast<u32_t>(x) * kHashPhi));
    h       = HashMix(h ^ (static_cast<u32_t>(y) * 0x85EBCA6Bu));
    return h;
}

[[nodiscard]] u32_t HashLattice3(i32_t x, i32_t y, i32_t z, u32_t seed) {
    u32_t h = seed;
    h       = HashMix(h ^ (static_cast<u32_t>(x) * kHashPhi));
    h       = HashMix(h ^ (static_cast<u32_t>(y) * 0x85EBCA6Bu));
    h       = HashMix(h ^ (static_cast<u32_t>(z) * 0xC2B2AE35u));
    return h;
}

void LatticeGradient2(i32_t x, i32_t y, u32_t seed, f32_t& gx, f32_t& gy) {
    constexpr f32_t k = 0.70710678118654752f;
    switch (HashLattice2(x, y, seed) & 7u) {
        case 0: gx = 1.0f; gy = 0.0f; return;
        case 1: gx = -1.0f; gy = 0.0f; return;
        case 2: gx = 0.0f; gy = 1.0f; return;
        case 3: gx = 0.0f; gy = -1.0f; return;
        case 4: gx = k; gy = k; return;
        case 5: gx = -k; gy = k; return;
        case 6: gx = k; gy = -k; return;
        default: gx = -k; gy = -k; return;
    }
}

void LatticeGradient3(i32_t x, i32_t y, i32_t z, u32_t seed, std::array<f32_t, 3>& out) {
    static constexpr f32_t kTable[12][3] = {
        {1, 1, 0},  {-1, 1, 0},  {1, -1, 0}, {-1, -1, 0}, {1, 0, 1},  {-1, 0, 1},
        {1, 0, -1}, {-1, 0, -1}, {0, 1, 1},  {0, -1, 1},  {0, 1, -1}, {0, -1, -1},
    };
    const u32_t index = HashLattice3(x, y, z, seed) % 12u;
    out               = {kTable[index][0], kTable[index][1], kTable[index][2]};
}

/// value, d/dx, d/dy at `p`, in the units of `p`.
[[nodiscard]] std::array<f32_t, 3> Simplex2D(f32_t px, f32_t py, u32_t seed) {
    constexpr f32_t F2    = 0.36602540378443865f;
    constexpr f32_t G2    = 0.21132486540518712f;
    constexpr f32_t kScale = 99.0f;

    const f32_t skew   = (px + py) * F2;
    const f32_t cellXf = std::floor(px + skew);
    const f32_t cellYf = std::floor(py + skew);
    const f32_t unskew = (cellXf + cellYf) * G2;
    const f32_t d0x    = px - (cellXf - unskew);
    const f32_t d0y    = py - (cellYf - unskew);

    const f32_t step1x = d0x > d0y ? 1.0f : 0.0f;
    const f32_t step1y = d0x > d0y ? 0.0f : 1.0f;

    const i32_t cellX = static_cast<i32_t>(cellXf);
    const i32_t cellY = static_cast<i32_t>(cellYf);

    std::array<f32_t, 3> sum{};
    const auto corner = [&sum](f32_t dx, f32_t dy, f32_t gx, f32_t gy) {
        const f32_t w = 0.5f - (dx * dx + dy * dy);
        if (w <= 0.0f) {
            return;
        }
        const f32_t w2 = w * w;
        const f32_t w3 = w2 * w;
        const f32_t w4 = w2 * w2;
        const f32_t gd = gx * dx + gy * dy;
        sum[0] += w4 * gd;
        sum[1] += (-8.0f * w3 * gd) * dx + w4 * gx;
        sum[2] += (-8.0f * w3 * gd) * dy + w4 * gy;
    };

    f32_t gx = 0.0f;
    f32_t gy = 0.0f;
    LatticeGradient2(cellX, cellY, seed, gx, gy);
    corner(d0x, d0y, gx, gy);
    LatticeGradient2(cellX + static_cast<i32_t>(step1x), cellY + static_cast<i32_t>(step1y), seed,
                     gx, gy);
    corner(d0x - step1x + G2, d0y - step1y + G2, gx, gy);
    LatticeGradient2(cellX + 1, cellY + 1, seed, gx, gy);
    corner(d0x - 1.0f + 2.0f * G2, d0y - 1.0f + 2.0f * G2, gx, gy);

    return {sum[0] * kScale, sum[1] * kScale, sum[2] * kScale};
}

/// value, d/dx, d/dy, d/dz at `p`.
[[nodiscard]] std::array<f32_t, 4> Simplex3D(std::array<f32_t, 3> p, u32_t seed) {
    constexpr f32_t F3     = 0.33333333333333333f;
    constexpr f32_t G3     = 0.16666666666666666f;
    constexpr f32_t kScale = 78.0f;

    const f32_t skew = (p[0] + p[1] + p[2]) * F3;
    std::array<f32_t, 3> cell{std::floor(p[0] + skew), std::floor(p[1] + skew),
                              std::floor(p[2] + skew)};
    const f32_t unskew = (cell[0] + cell[1] + cell[2]) * G3;
    std::array<f32_t, 3> d0{p[0] - (cell[0] - unskew), p[1] - (cell[1] - unskew),
                            p[2] - (cell[2] - unskew)};

    std::array<f32_t, 3> step1{};
    std::array<f32_t, 3> step2{};
    if (d0[0] >= d0[1]) {
        if (d0[1] >= d0[2]) {
            step1 = {1, 0, 0};
            step2 = {1, 1, 0};
        } else if (d0[0] >= d0[2]) {
            step1 = {1, 0, 0};
            step2 = {1, 0, 1};
        } else {
            step1 = {0, 0, 1};
            step2 = {1, 0, 1};
        }
    } else {
        if (d0[1] < d0[2]) {
            step1 = {0, 0, 1};
            step2 = {0, 1, 1};
        } else if (d0[0] < d0[2]) {
            step1 = {0, 1, 0};
            step2 = {0, 1, 1};
        } else {
            step1 = {0, 1, 0};
            step2 = {1, 1, 0};
        }
    }

    std::array<f32_t, 4> sum{};
    const auto corner = [&sum](std::array<f32_t, 3> d, const std::array<f32_t, 3>& g) {
        const f32_t w = 0.5f - (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (w <= 0.0f) {
            return;
        }
        const f32_t w2 = w * w;
        const f32_t w3 = w2 * w;
        const f32_t w4 = w2 * w2;
        const f32_t gd = g[0] * d[0] + g[1] * d[1] + g[2] * d[2];
        sum[0] += w4 * gd;
        for (usize_t i = 0; i < 3; ++i) {
            sum[i + 1] += (-8.0f * w3 * gd) * d[i] + w4 * g[i];
        }
    };

    const std::array<i32_t, 3> base{static_cast<i32_t>(cell[0]), static_cast<i32_t>(cell[1]),
                                    static_cast<i32_t>(cell[2])};
    std::array<f32_t, 3>       gradient{};

    LatticeGradient3(base[0], base[1], base[2], seed, gradient);
    corner(d0, gradient);
    LatticeGradient3(base[0] + static_cast<i32_t>(step1[0]), base[1] + static_cast<i32_t>(step1[1]),
                     base[2] + static_cast<i32_t>(step1[2]), seed, gradient);
    corner({d0[0] - step1[0] + G3, d0[1] - step1[1] + G3, d0[2] - step1[2] + G3}, gradient);
    LatticeGradient3(base[0] + static_cast<i32_t>(step2[0]), base[1] + static_cast<i32_t>(step2[1]),
                     base[2] + static_cast<i32_t>(step2[2]), seed, gradient);
    corner({d0[0] - step2[0] + 2.0f * G3, d0[1] - step2[1] + 2.0f * G3,
            d0[2] - step2[2] + 2.0f * G3},
           gradient);
    LatticeGradient3(base[0] + 1, base[1] + 1, base[2] + 1, seed, gradient);
    corner({d0[0] - 1.0f + 3.0f * G3, d0[1] - 1.0f + 3.0f * G3, d0[2] - 1.0f + 3.0f * G3},
           gradient);

    return {sum[0] * kScale, sum[1] * kScale, sum[2] * kScale, sum[3] * kScale};
}

/// A base function sample: value plus its exact gradient, with `axes` components in use.
template <usize_t Axes>
struct BaseSample {
    f32_t                    value = 0.0f;
    std::array<f32_t, Axes>  gradient{};
};

/// Derived base functions, mirroring `Ridged2D`/`Billow2D` and their 3D forms.
///
/// At value == 0 the sign is taken as +1, which is the right-hand derivative. The GLSL side makes
/// the same choice for the same reason: `|n|` has no derivative there, and a deterministic
/// tie-break is what keeps a region split into sections bit-identical.
template <usize_t Axes>
void ShapeBase(NoiseKind kind, BaseSample<Axes>& sample) {
    if (kind == NoiseKind::Simplex) {
        return;
    }
    const f32_t sign = sample.value < 0.0f ? -1.0f : 1.0f;
    if (kind == NoiseKind::Ridged) {
        const f32_t r = 1.0f - sign * sample.value;
        sample.value  = r * r;
        for (usize_t i = 0; i < Axes; ++i) {
            sample.gradient[i] *= -2.0f * r * sign;
        }
        return;
    }
    sample.value = 2.0f * sign * sample.value - 1.0f;
    for (usize_t i = 0; i < Axes; ++i) {
        sample.gradient[i] *= 2.0f * sign;
    }
}

/// The fBm accumulation, generic over the base function, mirroring `ENGINE_DEFINE_FBM`.
///
/// The multiply grouping matches the GLSL exactly: one weight per octave, one amplitude multiply at
/// the end. Any other grouping changes the last bit, and `test_kernels` compares the two.
template <usize_t Axes, typename Base>
[[nodiscard]] BaseSample<Axes> Fbm(const Graph::NoiseParams& params, u32_t seed,
                                   const std::array<f32_t, Axes>& point, Base base) {
    BaseSample<Axes> sum;
    f32_t            a    = 1.0f;
    f32_t            f    = params.frequency;
    f32_t            norm = 0.0f;

    for (u32_t octave = 0; octave < params.octaves; ++octave) {
        std::array<f32_t, Axes> scaled{};
        for (usize_t i = 0; i < Axes; ++i) {
            scaled[i] = point[i] * f;
        }
        BaseSample<Axes> n = base(scaled, seed + octave * kHashPhi);
        ShapeBase(params.kind, n);

        const f32_t af = a * f;
        sum.value += n.value * a;
        for (usize_t i = 0; i < Axes; ++i) {
            sum.gradient[i] += n.gradient[i] * af;
        }
        norm += a;
        a *= params.persistence;
        f *= params.lacunarity;
    }

    BaseSample<Axes> result;
    if (params.normalize) {
        result.value = params.amplitude * (sum.value / norm);
        for (usize_t i = 0; i < Axes; ++i) {
            result.gradient[i] = params.amplitude * (sum.gradient[i] / norm);
        }
    } else {
        result.value = params.amplitude * sum.value;
        for (usize_t i = 0; i < Axes; ++i) {
            result.gradient[i] = params.amplitude * sum.gradient[i];
        }
    }
    return result;
}

} // namespace

const OpInfo& OpInfoOf(OpKind kind) noexcept {
    const usize_t index = static_cast<usize_t>(kind);
    ENGINE_ASSERT(index < ArrayCount(kOps), "op {} is not in the registry", index);
    return kOps[index < ArrayCount(kOps) ? index : 0];
}

b8_t CanFold(OpKind kind) noexcept { return OpInfoOf(kind).hasReference; }

usize_t ResolutionParamWord(OpKind kind) noexcept { return OpInfoOf(kind).resolutionWord; }

b8_t EvalPointwise(const Graph::Node& node,
                   const std::array<std::array<f32_t, kMaxComponents>, kMaxNodeInputs>& inputs,
                   const std::array<u8_t, kMaxNodeInputs>& inputComponents,
                   std::array<f32_t, kMaxComponents>&      out) {
    out.fill(0.0f);
    const u8_t outComponents = node.channels[0].components;

    switch (node.kind) {
        case OpKind::Const:
            for (u8_t c = 0; c < outComponents; ++c) {
                out[c] = UnpackFloat(node.params, c);
            }
            return true;

        case OpKind::Arith: {
            const auto op = static_cast<ArithOp>(node.variant);
            for (u8_t c = 0; c < outComponents; ++c) {
                const f32_t a = inputs[0][inputComponents[0] == 1 ? 0 : c];
                const f32_t b = inputs[1][inputComponents[1] == 1 ? 0 : c];
                out[c]        = ApplyArith(op, a, b);
            }
            return true;
        }

        case OpKind::Curve: {
            const auto  op = static_cast<CurveOp>(node.variant);
            const f32_t a  = UnpackFloat(node.params, 0);
            const f32_t b  = UnpackFloat(node.params, 1);
            for (u8_t c = 0; c < outComponents; ++c) {
                out[c] = ApplyCurve(op, inputs[0][c], a, b);
            }
            return true;
        }

        case OpKind::Blend: {
            f32_t t = inputs[2][0];
            t       = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            for (u8_t c = 0; c < outComponents; ++c) {
                out[c] = inputs[0][c] + (inputs[1][c] - inputs[0][c]) * t;
            }
            return true;
        }

        case OpKind::Normals: {
            const f32_t scale = UnpackFloat(node.params, 0);
            const f32_t sx    = inputs[0][0] * scale;
            const f32_t sz    = inputs[0][1] * scale;
            const f32_t length = std::sqrt(sx * sx + 1.0f + sz * sz);
            out[0]            = -sx / length;
            out[1]            = 1.0f / length;
            out[2]            = -sz / length;
            return true;
        }

        case OpKind::SlopeMask: {
            const f32_t slope =
                std::sqrt(inputs[0][0] * inputs[0][0] + inputs[0][1] * inputs[0][1]);
            out[0] = Smoothstep(UnpackFloat(node.params, 0), UnpackFloat(node.params, 1), slope);
            return true;
        }

        case OpKind::Vector: {
            if (static_cast<VectorOp>(node.variant) == VectorOp::Extract) {
                out[0] = inputs[0][node.params[0]];
            } else {
                for (u8_t c = 0; c < outComponents; ++c) {
                    out[c] = inputs[c][0];
                }
            }
            return true;
        }

        case OpKind::Coords:
        case OpKind::Noise:
        case OpKind::Blur:
        case OpKind::Count:
            // Both depend on world position, which a pointwise reference does not receive.
            return false;
    }
    return false;
}

NoiseSample EvalNoise(const Graph::NoiseParams& params, u32_t seed,
                      std::array<f32_t, 3> position) {
    NoiseSample result;

    if (params.domain == Domain::R3) {
        const BaseSample<3> sample = Fbm<3>(
            params, seed, position, [](const std::array<f32_t, 3>& p, u32_t octaveSeed) {
                const std::array<f32_t, 4> n = Simplex3D(p, octaveSeed);
                return BaseSample<3>{.value = n[0], .gradient = {n[1], n[2], n[3]}};
            });
        result.value    = sample.value + params.offset;
        result.gradient = sample.gradient;
        return result;
    }

    // R2 lives in the (x, z) plane, so the y component of `position` is ignored.
    const BaseSample<2> sample = Fbm<2>(
        params, seed, {position[0], position[2]},
        [](const std::array<f32_t, 2>& p, u32_t octaveSeed) {
            const std::array<f32_t, 3> n = Simplex2D(p[0], p[1], octaveSeed);
            return BaseSample<2>{.value = n[0], .gradient = {n[1], n[2]}};
        });
    result.value       = sample.value + params.offset;
    result.gradient[0] = sample.gradient[0];
    result.gradient[1] = sample.gradient[1];
    return result;
}

// --- KernelLibrary --------------------------------------------------------------------------------

Result<KernelLibrary> KernelLibrary::Create(vulkan::Context& context) {
    KernelLibrary library;
    library.m_context = &context;
    return library;
}

Result<const vulkan::ComputePipeline*> KernelLibrary::Get(OpKind kind, u32_t variant,
                                                         Domain domain) {
    for (usize_t i = 0; i < m_count; ++i) {
        if (m_entries[i].kind == kind && m_entries[i].variant == variant
            && m_entries[i].domain == domain) {
            return &m_entries[i].pipeline;
        }
    }
    if (m_count == kMaxPipelines) {
        ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Compile,
                    "the kernel library already holds {} pipelines", kMaxPipelines);
    }
    ENGINE_ASSERT_RETURN(std::unexpected(MakeError(ErrorCode::InternalError, ErrorStage::Compile,
                                                  "the kernel library has no context")),
                         m_context != nullptr, "KernelLibrary was not created");

#ifdef TRACY_ENABLE
    ZoneScopedN("kernels: pipeline creation");
#endif
    const OpInfo& info = OpInfoOf(kind);

    // Ids 0 to 2 are the workgroup size, so one module serves both domains; id 3 is the variant,
    // which is how a change of code shape avoids any shader compilation.
    vulkan::SpecializationValues specialization =
        vulkan::WorkgroupSpecialization(static_cast<u32_t>(domain));
    for (u8_t field = 0; field < info.specFieldCount; ++field) {
        specialization.Add((variant >> (field * kVariantFieldBits)) & kVariantFieldMask);
    }

    const Clock::time_point start = Clock::now();
    Result<vulkan::ComputePipeline> pipeline = vulkan::ComputePipeline::Create(
        m_context->Device(), info.shader, specialization, m_context->Pipelines().Handle());
    if (!pipeline) {
        return std::unexpected(pipeline.error());
    }
    m_creationMs += Milliseconds(Clock::now() - start).count();

    Entry& entry   = m_entries[m_count++];
    entry.kind     = kind;
    entry.variant  = variant;
    entry.domain   = domain;
    entry.pipeline = std::move(*pipeline);
    LOG_DEBUG("kernel {} variant {} ({}) ready", info.name, variant,
              domain == Domain::R3 ? "R3" : "R2");
    return &entry.pipeline;
}

} // namespace engine::terrain
