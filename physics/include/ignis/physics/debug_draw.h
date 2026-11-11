#pragma once

#include "ignis/math/math.h"
#include "ignis/physics/physics_world.h"

#include <utility>
#include <vector>

namespace ignis {

// One world-space line segment for editor debug visualization. The renderer
// consumes these as DebugLine (renderer module); generating the geometry here
// in physics keeps it unit-testable headless and keeps the editor free of
// renderer-specific debug details.
struct DebugSegment {
    math::Vec3 a, b;
};

// Wireframe edges of a collider in world space. `world` is the collider's
// full transform (entity hierarchy and rotation included). Edge counts:
// Box 12, Sphere 12 (3 great circles x 4 segments), Capsule 24.
std::vector<DebugSegment> collider_wireframe_edges(const ColliderComponent& c, const math::Mat4& world);

// The 12 edges of an axis-aligned box.
std::vector<DebugSegment> aabb_edges(const math::Vec3& center, const math::Vec3& half);

} // namespace ignis
