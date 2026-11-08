#pragma once

#include "ignis/core/types.h"
#include "ignis/math/math.h"

#include <string>
#include <vector>

namespace ignis {

// One drawable primitive of a mesh. Vertex layout matches the gltf attributes:
// position, normal, texcoord0, tangent (optional).
struct MeshPrimitive {
    std::vector<math::Vec3> positions;
    std::vector<math::Vec3> normals;
    std::vector<math::Vec2> texcoords;
    std::vector<math::Vec4> tangents;
    std::vector<u32> indices;
    u32 material_index = 0;
};

struct Mesh {
    std::string name;
    std::vector<MeshPrimitive> primitives;
};

} // namespace ignis
