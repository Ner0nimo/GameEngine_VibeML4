#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 rt_out; // rgba8 sRGB

layout(set = 0, binding = 0) uniform sampler2D tex_hdr;
layout(set = 0, binding = 1) uniform UniformBuffer {
    float exposure;
    vec4 clearColor; // applied where alpha/hdr is background
    float _pad0, _pad1;
} ubo;

// ACES filmic tonemapping (Narkowicz approximation).
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(tex_hdr, v_uv).rgb;
    vec3 mapped = aces(hdr * ubo.exposure);
    // gamma correct (linear -> sRGB)
    vec3 gamma = pow(mapped, vec3(1.0 / 2.2));
    rt_out = vec4(gamma, 1.0);
}
