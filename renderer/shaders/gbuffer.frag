#version 450

layout(location = 0) in vec3 v_worldPos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec4 v_tangent;

// RT0: albedo (rgba8), RT1: normal*0.5+0.5 (rgba16f), RT2: ao/rough/metal (rgb8),
// RT3: emissive (rgba16f), RT4: world position (rgba16f)
layout(location = 0) out vec4 rt_albedo;
layout(location = 1) out vec4 rt_normal;
layout(location = 2) out vec4 rt_orm;
layout(location = 3) out vec4 rt_emissive;
layout(location = 4) out vec4 rt_worldPos;

layout(set = 1, binding = 0) uniform sampler2D tex_baseColor;
layout(set = 1, binding = 1) uniform sampler2D tex_metallicRoughness;
layout(set = 1, binding = 2) uniform sampler2D tex_normal;
layout(set = 1, binding = 3) uniform sampler2D tex_occlusion;
layout(set = 1, binding = 4) uniform sampler2D tex_emissive;

layout(push_constant) uniform Push {
    mat4 model;
    mat4 viewProj;
    vec4 baseColorFactor;
    vec2 metallicRoughness; // x = metallic, y = roughness
    vec4 emissive;         // rgb + strength
    uint texFlags;         // bit0 baseColor, 1 mr, 2 normal, 3 occlusion, 4 emissive
} pc;

void main() {
    vec4 base = pc.baseColorFactor;
    if ((pc.texFlags & 1u) != 0u) base *= texture(tex_baseColor, v_uv);

    float metallic = pc.metallicRoughness.x;
    float roughness = pc.metallicRoughness.y;
    if ((pc.texFlags & 2u) != 0u) {
        vec3 mr = texture(tex_metallicRoughness, v_uv).rgb; // glTF: g=roughness, b=metallic
        roughness *= mr.g;
        metallic *= mr.b;
    }

    vec3 N = normalize(v_normal);
    if ((pc.texFlags & 4u) != 0u) {
        vec3 T = normalize(v_tangent.xyz);
        vec3 B = cross(N, T) * v_tangent.w;
        vec3 nTS = texture(tex_normal, v_uv).xyz * 2.0 - 1.0;
        N = normalize(mat3(T, B, N) * nTS);
    }

    float ao = 1.0;
    if ((pc.texFlags & 8u) != 0u) ao = texture(tex_occlusion, v_uv).r;

    rt_albedo = vec4(base.rgb, base.a);
    rt_normal = vec4(N * 0.5 + 0.5, v_tangent.w);
    rt_orm = vec4(ao, roughness, metallic, 0.0);
    rt_emissive = vec4(pc.emissive.rgb * pc.emissive.a, 1.0);
    rt_worldPos = vec4(v_worldPos, 1.0);
}
