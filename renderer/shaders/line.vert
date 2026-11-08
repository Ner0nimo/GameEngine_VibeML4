#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;

layout(push_constant) uniform Push {
    mat4 viewProj;
} pc;

layout(location = 0) out vec4 v_color;

void main() {
    v_color = in_color;
    gl_Position = pc.viewProj * vec4(in_pos, 1.0);
}
