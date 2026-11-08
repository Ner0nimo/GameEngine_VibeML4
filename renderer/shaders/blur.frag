#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out float rt_ao;

layout(set = 0, binding = 0) uniform sampler2D tex_ao;
layout(set = 0, binding = 1) uniform UniformBuffer {
    vec2 texelSize; // 1/size
    vec2 direction; // (1,0) or (0,1)
    float _pad;
} ubo;

void main() {
    float sum = 0.0;
    float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    sum += texture(tex_ao, v_uv).r * w[0];
    for (int i = 1; i < 5; ++i) {
        vec2 off = ubo.direction * ubo.texelSize * float(i) * 1.5;
        sum += texture(tex_ao, v_uv + off).r * w[i];
        sum += texture(tex_ao, v_uv - off).r * w[i];
    }
    rt_ao = sum;
}
