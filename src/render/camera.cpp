#include <engine/render/camera.hpp>

#include <algorithm>

namespace engine::render {
namespace {

/// Radians per pixel of mouse movement. Chosen so a drag across a 1600-pixel window turns about 150
/// degrees, which is the range that feels like one gesture.
constexpr f32_t kTurnPerPixel = 0.0016f;

/// Just short of straight up or straight down. At exactly vertical the forward vector is parallel to
/// the up vector, the cross product that builds the view matrix collapses, and the view flips.
constexpr f32_t kPitchLimit = 1.5533f; // 89 degrees.

/// Multiplier per scroll tick. Geometric rather than linear so zooming feels the same at any distance.
constexpr f32_t kZoomPerTick = 1.15f;

constexpr f32_t kMinDistance = 0.5f;
constexpr f32_t kMaxDistance = 100000.0f;
constexpr f32_t kMinSpeed    = 0.5f;
constexpr f32_t kMaxSpeed    = 20000.0f;

} // namespace

Vec3 Camera::Forward() const noexcept {
    const f32_t cosPitch = std::cos(m_pitch);
    return Normalize(Vec3{std::sin(m_yaw) * cosPitch, std::sin(m_pitch),
                          std::cos(m_yaw) * cosPitch});
}

Vec3 Camera::Right() const noexcept {
    return Normalize(Cross(Forward(), Vec3{0.0f, 1.0f, 0.0f}));
}

Vec3 Camera::Position() const noexcept {
    // Orbit sits at a distance behind the target; fly keeps the target one unit ahead of the eye, so
    // switching modes does not teleport the camera.
    return m_mode == CameraMode::Orbit ? m_target - Forward() * m_distance : m_target;
}

Mat4 Camera::View() const noexcept {
    const Vec3 eye = Position();
    return LookAt(eye, eye + Forward(), Vec3{0.0f, 1.0f, 0.0f});
}

Mat4 Camera::Projection(f32_t aspect) const noexcept {
    return Perspective(m_fov, aspect, m_near, m_far);
}

void Camera::SetMode(CameraMode mode) noexcept {
    if (mode == m_mode) {
        return;
    }
    // The eye must not move. Orbit keeps its target in front; fly keeps the eye where it was.
    const Vec3 eye = Position();
    m_mode         = mode;
    if (mode == CameraMode::Fly) {
        m_target = eye;
    } else {
        m_target = eye + Forward() * m_distance;
    }
}

void Camera::Frame(Vec3 centre, f32_t radius) noexcept {
    m_target = centre;
    // Far enough that a sphere of `radius` fits in the vertical field of view, with a little margin.
    const f32_t fit = radius / std::tan(m_fov * 0.5f);
    m_distance      = std::clamp(fit * 1.2f, kMinDistance, kMaxDistance);
    if (m_mode == CameraMode::Fly) {
        m_target = centre - Forward() * m_distance;
    }
    // Clip planes scaled to what is being looked at: a fixed near plane of half a unit wastes most of
    // the depth range when the camera is kilometres away, and a fixed far plane clips the horizon when it
    // is metres away.
    SetClipPlanes(std::max(m_distance * 0.001f, 0.05f), std::max(m_distance * 20.0f, 1000.0f));
    m_speed = std::clamp(radius * 0.5f, kMinSpeed, kMaxSpeed);
}

void Camera::Turn(f32_t deltaYawPixels, f32_t deltaPitchPixels) noexcept {
    m_yaw -= deltaYawPixels * kTurnPerPixel;
    m_pitch = std::clamp(m_pitch - deltaPitchPixels * kTurnPerPixel, -kPitchLimit, kPitchLimit);
}

void Camera::Zoom(f32_t ticks) noexcept {
    const f32_t factor = std::pow(kZoomPerTick, -ticks);
    if (m_mode == CameraMode::Orbit) {
        m_distance = std::clamp(m_distance * factor, kMinDistance, kMaxDistance);
        SetClipPlanes(std::max(m_distance * 0.001f, 0.05f), std::max(m_distance * 20.0f, 1000.0f));
    } else {
        m_speed = std::clamp(m_speed * factor, kMinSpeed, kMaxSpeed);
    }
}

void Camera::Move(f32_t forward, f32_t right, f32_t up, f32_t deltaSeconds) noexcept {
    if (m_mode != CameraMode::Fly) {
        // Orbit has no free movement: panning moves the target instead, which is a different gesture.
        return;
    }
    const f32_t step = m_speed * deltaSeconds;
    m_target = m_target + Forward() * (forward * step) + Right() * (right * step)
               + Vec3{0.0f, up * step, 0.0f};
}

void Camera::Pan(f32_t deltaRightPixels, f32_t deltaUpPixels) noexcept {
    if (m_mode != CameraMode::Orbit) {
        return;
    }
    // Scaled by distance, so a drag moves the same amount of *screen* whatever the zoom level.
    const f32_t scale  = m_distance * kTurnPerPixel;
    const Vec3  right  = Right();
    const Vec3  trueUp = Cross(right, Forward());
    m_target = m_target - right * (deltaRightPixels * scale) - trueUp * (deltaUpPixels * scale);
}

void Camera::SetClipPlanes(f32_t near, f32_t far) noexcept {
    m_near = std::max(near, 1e-4f);
    m_far  = std::max(far, m_near * 10.0f);
}

} // namespace engine::render
