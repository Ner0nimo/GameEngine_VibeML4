#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 rt_out; // rgba8 sRGB

// NVRHI Vulkan binding convention: SRV at binding=slot, sampler at 128+slot, CB at 256+slot.
layout(set = 0, binding = 0) uniform texture2D tex_hdr;
layout(set = 0, binding = 128) uniform sampler samp_hdr;
// Bloom mip chain (bright pass + downsamples), composited in HDR below.
layout(set = 0, binding = 1) uniform texture2D tex_bloom0;
layout(set = 0, binding = 129) uniform sampler samp_bloom0;
layout(set = 0, binding = 2) uniform texture2D tex_bloom1;
layout(set = 0, binding = 130) uniform sampler samp_bloom1;
layout(set = 0, binding = 3) uniform texture2D tex_bloom2;
layout(set = 0, binding = 131) uniform sampler samp_bloom2;
layout(set = 0, binding = 4) uniform texture2D tex_bloom3;
layout(set = 0, binding = 132) uniform sampler samp_bloom3;
layout(set = 0, binding = 5) uniform texture2D tex_bloom4;
layout(set = 0, binding = 133) uniform sampler samp_bloom4;
layout(set = 0, binding = 6) uniform texture2D tex_ssr; // screen-space reflections
layout(set = 0, binding = 134) uniform sampler samp_ssr;
layout(set = 0, binding = 263) uniform UniformBuffer {
    float exposure;
    float bloomStrength; // 0 disables bloom
    float ssrStrength;   // 0 disables the SSR add
    float _padB;
    vec4 clearColor; // applied where alpha/hdr is background
} ubo;

// ACES filmic tonemapping (Narkowicz approximation).
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(sampler2D(tex_hdr, samp_hdr), v_uv).rgb;
    // SSR reflections add in HDR, before tonemapping.
    if (ubo.ssrStrength > 0.0)
        hdr += ubo.ssrStrength * texture(sampler2D(tex_ssr, samp_ssr), v_uv).rgb;
    // Bloom is composited in HDR, before tonemapping. Guard on bloomStrength so
    // a disabled bloom never samples the (then unwritten) bloom targets.
    if (ubo.bloomStrength > 0.0) {
        const float w[5] = float[](0.4, 0.25, 0.15, 0.12, 0.08);
        vec3 bloom = w[0] * texture(sampler2D(tex_bloom0, samp_bloom0), v_uv).rgb
                   + w[1] * texture(sampler2D(tex_bloom1, samp_bloom1), v_uv).rgb
                   + w[2] * texture(sampler2D(tex_bloom2, samp_bloom2), v_uv).rgb
                   + w[3] * texture(sampler2D(tex_bloom3, samp_bloom3), v_uv).rgb
                   + w[4] * texture(sampler2D(tex_bloom4, samp_bloom4), v_uv).rgb;
        hdr += ubo.bloomStrength * bloom;
    }
    vec3 mapped = aces(hdr * ubo.exposure);
    // gamma correct (linear -> sRGB)
    vec3 gamma = pow(mapped, vec3(1.0 / 2.2));
    rt_out = vec4(gamma, 1.0);
}
