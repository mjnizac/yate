#pragma once

#include <engine/common.hpp>

#include <array>
#include <cmath>

/// Just enough linear algebra for a camera, and a camera.
///
/// No GLM: the engine needs one 4x4 multiply, one perspective, one look-at and a normalize, and a
/// dependency whose headers are larger than the whole render module would be paying for a library to
/// use four of its functions. What is here is small enough to read in one sitting and has no
/// conventions to look up.
///
/// Conventions, stated once because every sign error in a renderer comes from leaving them implicit:
///  - Right-handed world space. `x` east, `y` up, `z` south, which matches the terrain domain where
///    `R2` is `(x, z)` and height is `y`.
///  - Column-vector convention: `v' = M * v`, and matrices are stored column-major, so `m[c][r]` is
///    column `c`, row `r`. That is the layout GLSL expects, so a matrix goes into push constants
///    without a transpose.
///  - Clip space is Vulkan's: depth in [0, 1] and `y` pointing down the screen. The projection does
///    both, so nothing downstream has to remember to flip anything.
namespace engine::render {

struct Vec3 {
    f32_t x = 0.0f;
    f32_t y = 0.0f;
    f32_t z = 0.0f;
};

[[nodiscard]] constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
[[nodiscard]] constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
[[nodiscard]] constexpr Vec3 operator*(Vec3 v, f32_t s) noexcept {
    return {v.x * s, v.y * s, v.z * s};
}
[[nodiscard]] constexpr f32_t Dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
[[nodiscard]] constexpr Vec3 Cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline f32_t Length(Vec3 v) noexcept { return std::sqrt(Dot(v, v)); }

/// Normalizes, or returns the zero vector when there is nothing to normalize.
///
/// Returning zero rather than a NaN: a zero-length direction happens when a camera is told to look at
/// its own position, and a NaN would propagate silently into the matrix and blank the screen.
[[nodiscard]] inline Vec3 Normalize(Vec3 v) noexcept {
    const f32_t length = Length(v);
    return length > 1e-12f ? v * (1.0f / length) : Vec3{};
}

/// Column-major 4x4 matrix. `m[c][r]` is column `c`, row `r`.
struct Mat4 {
    std::array<std::array<f32_t, 4>, 4> m{};

    [[nodiscard]] static constexpr Mat4 Identity() noexcept {
        Mat4 result;
        result.m[0][0] = 1.0f;
        result.m[1][1] = 1.0f;
        result.m[2][2] = 1.0f;
        result.m[3][3] = 1.0f;
        return result;
    }
};

[[nodiscard]] constexpr Mat4 operator*(const Mat4& a, const Mat4& b) noexcept {
    Mat4 result;
    for (usize_t c = 0; c < 4; ++c) {
        for (usize_t r = 0; r < 4; ++r) {
            f32_t sum = 0.0f;
            for (usize_t k = 0; k < 4; ++k) {
                sum += a.m[k][r] * b.m[c][k];
            }
            result.m[c][r] = sum;
        }
    }
    return result;
}

/// Right-handed look-at.
[[nodiscard]] inline Mat4 LookAt(Vec3 eye, Vec3 target, Vec3 up) noexcept {
    const Vec3 forward = Normalize(target - eye);
    const Vec3 right   = Normalize(Cross(forward, up));
    const Vec3 trueUp  = Cross(right, forward);

    Mat4 result = Mat4::Identity();
    result.m[0][0] = right.x;
    result.m[1][0] = right.y;
    result.m[2][0] = right.z;
    result.m[0][1] = trueUp.x;
    result.m[1][1] = trueUp.y;
    result.m[2][1] = trueUp.z;
    // Negated because the camera looks down -z in view space.
    result.m[0][2] = -forward.x;
    result.m[1][2] = -forward.y;
    result.m[2][2] = -forward.z;
    result.m[3][0] = -Dot(right, eye);
    result.m[3][1] = -Dot(trueUp, eye);
    result.m[3][2] = Dot(forward, eye);
    return result;
}

/// Perspective projection for Vulkan: depth in [0, 1] and `y` down.
///
/// Reversed depth, so near is 1 and far is 0. A float depth buffer has most of its precision near zero,
/// and a reversed range puts that precision where the geometry is close; with terrain stretching to the
/// horizon the alternative is visible z-fighting in the distance. The render pass clears depth to 0 and
/// the pipeline compares with `GREATER_OR_EQUAL` to match.
[[nodiscard]] inline Mat4 Perspective(f32_t verticalFovRadians, f32_t aspect, f32_t near,
                                      f32_t far) noexcept {
    const f32_t focal = 1.0f / std::tan(verticalFovRadians * 0.5f);

    Mat4 result;
    result.m[0][0] = focal / (aspect > 1e-6f ? aspect : 1e-6f);
    // Negative, which is the `y` flip: Vulkan's clip space has `y` pointing down the screen.
    result.m[1][1] = -focal;
    result.m[2][2] = near / (far - near);
    result.m[2][3] = -1.0f;
    result.m[3][2] = (far * near) / (far - near);
    return result;
}

/// How the camera responds to input.
enum class CameraMode : u32_t {
    /// Rotates around a target at a distance. What you want for looking at a piece of terrain.
    Orbit = 0,
    /// Moves where it is pointing. What you want for being in the terrain.
    Fly,
};

/// Orbit and fly in one object, because they share a position and an orientation and switching between
/// them should not move the camera.
///
/// The orientation is stored as yaw and pitch rather than as a quaternion or a matrix: those are the two
/// degrees of freedom either mode offers, pitch has to be clamped to avoid flipping over the pole, and
/// keeping the representation minimal means there is no third state to get out of step.
class Camera {
public:
    [[nodiscard]] Mat4 View() const noexcept;
    [[nodiscard]] Mat4 Projection(f32_t aspect) const noexcept;
    [[nodiscard]] Mat4 ViewProjection(f32_t aspect) const noexcept {
        return Projection(aspect) * View();
    }

    /// Where the camera is, in world units.
    [[nodiscard]] Vec3 Position() const noexcept;
    [[nodiscard]] Vec3 Forward() const noexcept;
    [[nodiscard]] Vec3 Right() const noexcept;

    void SetMode(CameraMode mode) noexcept;
    [[nodiscard]] CameraMode Mode() const noexcept { return m_mode; }

    /// Frames a region: centres the target on it and pulls back far enough to see all of it.
    void Frame(Vec3 centre, f32_t radius) noexcept;

    /// Turns by a pixel delta. Pitch is clamped just short of vertical, because at exactly vertical the
    /// up vector and the forward vector are parallel and the view matrix degenerates.
    void Turn(f32_t deltaYawPixels, f32_t deltaPitchPixels) noexcept;

    /// Orbit: moves closer or further. Fly: changes the movement speed, because there is no distance to
    /// change.
    void Zoom(f32_t ticks) noexcept;

    /// Moves in the camera's own frame. `forward`, `right` and `up` are in [-1, 1].
    void Move(f32_t forward, f32_t right, f32_t up, f32_t deltaSeconds) noexcept;

    /// Drags the orbit target across the view plane.
    void Pan(f32_t deltaRightPixels, f32_t deltaUpPixels) noexcept;

    [[nodiscard]] f32_t Distance() const noexcept { return m_distance; }
    [[nodiscard]] f32_t NearPlane() const noexcept { return m_near; }
    [[nodiscard]] f32_t FarPlane() const noexcept { return m_far; }
    [[nodiscard]] f32_t Speed() const noexcept { return m_speed; }

    void SetClipPlanes(f32_t near, f32_t far) noexcept;

private:
    CameraMode m_mode = CameraMode::Orbit;
    /// Orbit target, and in fly mode the point directly in front of the eye.
    Vec3  m_target{};
    f32_t m_distance = 100.0f;
    f32_t m_yaw      = 0.6f;
    f32_t m_pitch    = -0.5f;
    f32_t m_fov      = 1.0471975512f; // 60 degrees.
    f32_t m_near     = 0.5f;
    f32_t m_far      = 20000.0f;
    /// World units per second in fly mode.
    f32_t m_speed = 60.0f;
};

} // namespace engine::render
