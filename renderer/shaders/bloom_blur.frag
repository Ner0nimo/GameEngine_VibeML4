#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 rt_out; // rgba16f downsampled+blurred level

// NVRHI Vulkan binding convention: SRV at binding=slot, sampler at 128+slot, CB at 256+slot.
layout(set = 0, binding = 0) uniform texture2D tex_src; // previous (larger) bloom level
layout(set = 0, binding = 128) uniform sampler samp_src;
layout(set = 0, binding = 257) uniform UniformBuffer {
    vec2 texelSize; // 1 / source level size
    vec2 _pad0;
    float _pad1;
} ubo;

// Wide separable-style downsample kernel (center + axis/diagonal taps);
// linear filtering does the actual downsampling.
void main() {
    const float w0 = 0.227027;
    const float w1 = 0.1945946;
    const float w2 = 0.1216216;
    const float w3 = 0.054054;
    const float w4 = 0.016216;
    vec3 sum = texture(sampler2D(tex_src, samp_src), v_uv).rgb * w0;
    vec2 ts = ubo.texelSize * 1.5;
    sum += texture(sampler2D(tex_src, samp_src), v_uv + vec2(ts.x, 0.0)).rgb * w1;
    sum += texture(sampler2D(tex_src, samp_src), v_uv - vec2(ts.x, 0.0)).rgb * w1;
    sum += texture(sampler2D(tex_src, samp_src), v_uv + vec2(0.0, ts.y)).rgb * w1;
    sum += texture(sampler2D(tex_src, samp_src), v_uv - vec2(0.0, ts.y)).rgb * w1;
    sum += texture(sampler2D(tex_src, samp_src), v_uv + ts).rgb * w2;
    sum += texture(sampler2D(tex_src, samp_src), v_uv - ts).rgb * w2;
    sum += texture(sampler2D(tex_src, samp_src), v_uv + vec2(ts.x, -ts.y)).rgb * w2;
    sum += texture(sampler2D(tex_src, samp_src), v_uv + vec2(-ts.x, ts.y)).rgb * w2;
    sum += texture(sampler2D(tex_src, samp_src), v_uv + 2.0 * ts).rgb * w3;
    sum += texture(sampler2D(tex_src, samp_src), v_uv - 2.0 * ts).rgb * w3;
    sum += texture(sampler2D(tex_src, samp_src), v_uv + vec2(2.0 * ts.x, 0.0)).rgb * w3;
    sum += texture(sampler2D(tex_src, samp_src), v_uv - vec2(2.0 * ts.x, 0.0)).rgb * w3;
    sum += texture(sampler2D(tex_src, samp_src), v_uv + vec2(0.0, 2.0 * ts.y)).rgb * w3;
    sum += texture(sampler2D(tex_src, samp_src), v_uv - vec2(0.0, 2.0 * ts.y)).rgb * w3;
    sum += texture(sampler2D(tex_src, samp_src), v_uv + 3.0 * ts).rgb * w4;
    sum += texture(sampler2D(tex_src, samp_src), v_uv - 3.0 * ts).rgb * w4;
    rt_out = vec4(sum, 1.0);
}
