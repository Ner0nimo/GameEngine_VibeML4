#pragma once

#include "ignis/core/types.h"
#include "ignis/math/math.h"

#include <string>

namespace ignis {

// PBR metallic-roughness material (glTF 2.0 model).
struct Material {
    std::string name;
    math::Vec4 base_color{1.f, 1.f, 1.f, 1.f};
    f32 metallic = 0.f;
    f32 roughness = 0.5f;
    f32 ao = 1.f;
    math::Vec3 emissive{0.f, 0.f, 0.f};
    f32 emissive_strength = 0.f;
    bool double_sided = false;

    // Texture slots; -1 = unused. Indices into Model::textures.
    int base_color_texture = -1;
    int metallic_roughness_texture = -1;
    int normal_texture = -1;
    int occlusion_texture = -1;
    int emissive_texture = -1;
};

} // namespace ignis
