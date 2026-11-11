#include "ignis/physics/debug_draw.h"

#include <cmath>

namespace ignis {

namespace {

math::Vec3 xform(const math::Mat4& m, const math::Vec3& p) {
    const math::Vec4 v = m * math::Vec4(p, 1.f);
    return math::Vec3(v);
}

// Appends `n` segments approximating a circle of `radius` in the plane given
// by the two axes, centered at `center`.
void add_circle(std::vector<DebugSegment>& out, const math::Vec3& center, f32 radius, int n,
                const math::Vec3& axis_u, const math::Vec3& axis_v, const math::Mat4& world) {
    const f32 step = 2.f * 3.14159265358979f / static_cast<f32>(n);
    math::Vec3 prev = center + axis_u * radius;
    for (int i = 1; i <= n; ++i) {
        const f32 ang = step * static_cast<f32>(i);
        const f32 ca = static_cast<f32>(std::cos(ang));
        const f32 sa = static_cast<f32>(std::sin(ang));
        const math::Vec3 p = center + axis_u * (radius * ca) + axis_v * (radius * sa);
        out.push_back({xform(world, prev), xform(world, p)});
        prev = p;
    }
}

void add_box_edges(std::vector<DebugSegment>& out, const math::Vec3& half, const math::Mat4& world) {
    const math::Vec3 c[8] = {
        {-half.x, -half.y, -half.z}, {half.x, -half.y, -half.z},
        {half.x, half.y, -half.z},    {-half.x, half.y, -half.z},
        {-half.x, -half.y, half.z},   {half.x, -half.y, half.z},
        {half.x, half.y, half.z},     {-half.x, half.y, half.z},
    };
    constexpr int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                  {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges) out.push_back({xform(world, c[e[0]]), xform(world, c[e[1]])});
}

} // namespace

std::vector<DebugSegment> collider_wireframe_edges(const ColliderComponent& c, const math::Mat4& world) {
    std::vector<DebugSegment> out;
    switch (c.shape) {
        case ColliderComponent::Shape::Box:
            add_box_edges(out, c.half_extents, world);
            break;
        case ColliderComponent::Shape::Sphere:
            // 3 great circles (XY, XZ, YZ), 4 segments each = 12 edges.
            add_circle(out, math::Vec3(0.f), c.radius, 4, math::Vec3(1.f, 0.f, 0.f),
                       math::Vec3(0.f, 1.f, 0.f), world);
            add_circle(out, math::Vec3(0.f), c.radius, 4, math::Vec3(1.f, 0.f, 0.f),
                       math::Vec3(0.f, 0.f, 1.f), world);
            add_circle(out, math::Vec3(0.f), c.radius, 4, math::Vec3(0.f, 1.f, 0.f),
                       math::Vec3(0.f, 0.f, 1.f), world);
            break;
        case ColliderComponent::Shape::Capsule: {
            // Cylinder part of `height` along Y, capped by the radius at both
            // ends: top/bottom circles (8 segments each) + 8 verticals = 24.
            const f32 half_h = c.height * 0.5f;
            const math::Vec3 up(0.f, 1.f, 0.f);
            add_circle(out, up * half_h, c.radius, 8, math::Vec3(1.f, 0.f, 0.f),
                       math::Vec3(0.f, 0.f, 1.f), world);
            add_circle(out, -up * half_h, c.radius, 8, math::Vec3(1.f, 0.f, 0.f),
                       math::Vec3(0.f, 0.f, 1.f), world);
            for (int i = 0; i < 8; ++i) {
                const f32 ang = 2.f * 3.14159265358979f * static_cast<f32>(i) / 8.f;
                const f32 ca = static_cast<f32>(std::cos(ang));
                const f32 sa = static_cast<f32>(std::sin(ang));
                const math::Vec3 dir = math::Vec3(ca, 0.f, sa) * c.radius;
                out.push_back({xform(world, dir - up * half_h), xform(world, dir + up * half_h)});
            }
            break;
        }
    }
    return out;
}

std::vector<DebugSegment> aabb_edges(const math::Vec3& center, const math::Vec3& half) {
    std::vector<DebugSegment> out;
    add_box_edges(out, half, glm::translate(math::Mat4(1.f), center));
    return out;
}

} // namespace ignis
