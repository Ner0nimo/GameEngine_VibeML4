#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 rt_bright; // rgba16f, HDR bright-pass

// NVRHI Vulkan binding convention: SRV at binding=slot, sampler at 128+slot, CB at 256+slot.
layout(set = 0, binding = 0) uniform texture2D tex_hdr;
layout(set = 0, binding = 128) uniform sampler samp_hdr;
layout(set = 0, binding = 257) uniform UniformBuffer {
    float threshold; // luminance above which a pixel blooms
    float knee;      // soft-knee width (0 = hard threshold)
    float _p0, _p1;
} ubo;

void main() {
    vec3 c = texture(sampler2D(tex_hdr, samp_hdr), v_uv).rgb;
    float l = max(c.r, max(c.g, c.b));
    // Soft knee: smoothstep-like weight around the threshold.
    float soft = clamp(l - ubo.threshold + ubo.knee, 0.0, 2.0 * ubo.knee);
    soft = soft * soft / max(4.0 * ubo.knee, 1e-4);
    float w = max(soft, l - ubo.threshold) / max(l, 1e-4);
    rt_bright = vec4(c * clamp(w, 0.0, 1.0), 1.0);
}
