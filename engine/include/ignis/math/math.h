#pragma once

#include "ignis/core/types.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>

namespace ignis::math {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using Quat = glm::quat;

constexpr f32 PI = glm::pi<f32>();

constexpr f32 radians(f32 degrees) noexcept { return glm::radians(degrees); }
constexpr f32 degrees(f32 radians_) noexcept { return glm::degrees(radians_); }

// Right-handed, column-major matrices (glm convention).
inline Mat4 translate(const Mat4& m, const Vec3& v) { return glm::translate(m, v); }
inline Mat4 scale(const Mat4& m, const Vec3& v) { return glm::scale(m, v); }
inline Mat4 rotate(const Mat4& m, f32 angle_rad, const Vec3& axis) { return glm::rotate(m, angle_rad, axis); }

inline Mat4 look_at(const Vec3& eye, const Vec3& center, const Vec3& up) {
    return glm::lookAt(eye, center, up);
}

// Perspective projection, depth range [0, 1] as used by Vulkan.
inline Mat4 perspective_vk(f32 fov_y_rad, f32 aspect, f32 z_near, f32 z_far) {
    Mat4 m = glm::perspective(fov_y_rad, aspect, z_near, z_far);
    m[2][2] = z_far / (z_near - z_far);
    m[3][2] = (z_near * z_far) / (z_near - z_far);
    return m;
}

// Orthographic projection, depth range [0, 1] as used by Vulkan.
inline Mat4 ortho_vk(f32 left, f32 right, f32 bottom, f32 top, f32 z_near, f32 z_far) {
    Mat4 m = glm::ortho(left, right, bottom, top, z_near, z_far);
    m[2][2] = 1.f / (z_near - z_far);
    m[3][2] = z_near / (z_near - z_far);
    return m;
}

inline Vec3 forward(const Quat& q) { return glm::rotate(q, Vec3(0.f, 0.f, -1.f)); }
inline Vec3 right(const Quat& q)   { return glm::rotate(q, Vec3(1.f, 0.f, 0.f)); }
inline Vec3 up(const Quat& q)      { return glm::rotate(q, Vec3(0.f, 1.f, 0.f)); }

} // namespace ignis::math
