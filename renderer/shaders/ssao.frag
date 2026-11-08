#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out float rt_ao;

layout(set = 0, binding = 0) uniform sampler2D tex_depth;   // depth (r)
layout(set = 0, binding = 1) uniform sampler2D tex_normal;  // rgba16f, normal*0.5+0.5
layout(set = 0, binding = 2) uniform UniformBuffer {
    mat4 proj;
    mat4 invProj;
    vec4 kernel[32];      // xyz = sample dir (hemisphere, z up), w = scale
    vec2 noiseScale;      // screen / noise size
    float radius;
    float power;
    vec2 screenSize;
    float _pad;
} ubo;

vec3 viewPosFromDepth(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 v = ubo.invProj * clip;
    return v.xyz / v.w;
}

void main() {
    float depth = texture(tex_depth, v_uv).r;
    if (depth >= 1.0) { rt_ao = 1.0; return; } // sky: no occlusion
    vec3 P = viewPosFromDepth(v_uv, depth);
    vec3 N = normalize(texture(tex_normal, v_uv).xyz * 2.0 - 1.0);

    // Noise rotation (4x4 tiled).
    vec2 noiseUv = v_uv * ubo.noiseScale;
    // procedural hash noise to avoid a noise texture binding
    float n = fract(sin(dot(noiseUv, vec2(12.9898, 78.233))) * 43758.5453);
    mat2 rot = mat2(cos(n * 6.2831), -sin(n * 6.2831), sin(n * 6.2831), cos(n * 6.2831));

    float occlusion = 0.0;
    for (int i = 0; i < 32; ++i) {
        vec3 s = vec3(rot * ubo.kernel[i].xy, ubo.kernel[i].z) * ubo.kernel[i].w;
        vec3 samplePos = P + s * ubo.radius;
        vec4 clip = ubo.proj * vec4(samplePos, 1.0);
        vec2 suv = clip.xy / clip.w * 0.5 + 0.5;
        float sampleDepth = texture(tex_depth, suv).r;
        vec3 sp = viewPosFromDepth(suv, sampleDepth);
        float range = clamp(1.0 - abs(P.z - sp.z) / ubo.radius, 0.0, 1.0);
        occlusion += (sp.z <= samplePos.z ? 1.0 : 0.0) * range;
    }
    occlusion = 1.0 - occlusion / 32.0;
    rt_ao = pow(clamp(occlusion, 0.0, 1.0), ubo.power);
}
