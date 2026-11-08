#include <gtest/gtest.h>

#include "ignis/math/math.h"

using namespace ignis;

TEST(Math, VecQuatBasics) {
    const math::Quat q = glm::angleAxis(math::radians(90.f), math::Vec3(0, 1, 0));
    const math::Vec3 f = math::forward(q); // -Z rotated +90 deg about Y -> -X
    EXPECT_NEAR(f.x, -1.f, 1e-5f);
    EXPECT_NEAR(f.y, 0.f, 1e-5f);
    EXPECT_NEAR(f.z, 0.f, 1e-5f);
    const math::Vec3 r = math::right(q); // +X rotated +90 deg about Y -> -Z
    EXPECT_NEAR(glm::length(r - math::Vec3(0.f, 0.f, -1.f)), 0.f, 1e-5f);
}

TEST(Math, PerspectiveVkDepthRange) {
    const math::Mat4 p = math::perspective_vk(math::radians(60.f), 16.f / 9.f, 0.1f, 100.f);
    // Vulkan depth [0,1]: a point at z=-near maps to clip z = 0 after divide.
    const glm::vec4 clip = p * glm::vec4(0.f, 0.f, -0.1f, 1.f);
    EXPECT_NEAR(clip.z / clip.w, 0.f, 1e-4f);
    const glm::vec4 clip_far = p * glm::vec4(0.f, 0.f, -100.f, 1.f);
    EXPECT_NEAR(clip_far.z / clip_far.w, 1.f, 1e-3f);
}

TEST(Math, LookAt) {
    const math::Mat4 v = math::look_at({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    const glm::vec4 origin_view = v * glm::vec4(0, 0, 0, 1);
    EXPECT_NEAR(origin_view.z, -5.f, 1e-5f);
}
