#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tangent;

layout(push_constant) uniform Push {
    mat4 model;
    mat4 viewProj;
} pc;

layout(location = 0) out vec3 v_worldPos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec4 v_tangent;

void main() {
    vec4 wp = pc.model * vec4(in_pos, 1.0);
    v_worldPos = wp.xyz;
    v_normal = mat3(pc.model) * in_normal; // uniform scale assumed (v1)
    v_uv = in_uv;
    v_tangent = vec4(mat3(pc.model) * in_tangent.xyz, in_tangent.w);
    gl_Position = pc.viewProj * wp;
}
