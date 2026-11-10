#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 rt_ssr; // rgba16f: reflected scene color * F0 * confidence

// NVRHI Vulkan binding convention: SRV at binding=slot, sampler at 128+slot, CB at 256+slot.
layout(set = 0, binding = 0) uniform texture2D tex_hdr;      // lit scene color (post-lighting)
layout(set = 0, binding = 128) uniform sampler samp_hdr;
layout(set = 0, binding = 1) uniform texture2D tex_depth;    // gbuffer depth
layout(set = 0, binding = 129) uniform sampler samp_depth;
layout(set = 0, binding = 2) uniform texture2D tex_normal;
layout(set = 0, binding = 130) uniform sampler samp_normal;
layout(set = 0, binding = 3) uniform texture2D tex_orm;
layout(set = 0, binding = 131) uniform sampler samp_orm;
layout(set = 0, binding = 4) uniform texture2D tex_worldPos;
layout(set = 0, binding = 132) uniform sampler samp_worldPos;
layout(set = 0, binding = 5) uniform texture2D tex_albedo;
layout(set = 0, binding = 133) uniform sampler samp_albedo;
layout(set = 0, binding = 262) uniform UniformBuffer {
    mat4 camView;
    mat4 camProj;
    mat4 camInvProj;
    vec4 camPos;  // xyz = camera position
    vec4 params;  // x = enabled, y = strength, z = max roughness, w = max ray distance (view units)
} ubo;

// View-space z of the scene point stored at depth d and uv.
float sceneViewZ(vec2 uv, float d) {
    vec4 sc = ubo.camInvProj * vec4(uv * 2.0 - 1.0, d, 1.0);
    return sc.z / sc.w;
}

// Project a view-space point to screen uv (returns false if behind the camera).
bool project(vec3 p, out vec2 uv) {
    vec4 clip = ubo.camProj * vec4(p, 1.0);
    if (clip.w <= 0.0) return false;
    uv = clip.xy / clip.w * 0.5 + 0.5;
    return true;
}

void main() {
    float depth = texture(sampler2D(tex_depth, samp_depth), v_uv).r;
    if (depth >= 1.0) { rt_ssr = vec4(0.0); return; } // sky: no reflection
    float roughness = clamp(texture(sampler2D(tex_orm, samp_orm), v_uv).g, 0.04, 1.0);
    float metallic = clamp(texture(sampler2D(tex_orm, samp_orm), v_uv).b, 0.0, 1.0);
    if (ubo.params.x < 0.5 || roughness > ubo.params.z) { rt_ssr = vec4(0.0); return; }

    vec3 worldPos = texture(sampler2D(tex_worldPos, samp_worldPos), v_uv).xyz;
    vec3 N = normalize(texture(sampler2D(tex_normal, samp_normal), v_uv).xyz * 2.0 - 1.0);
    vec3 viewPos = vec3(ubo.camView * vec4(worldPos, 1.0));
    vec3 Nv = normalize(mat3(ubo.camView) * N);
    vec3 Rv = reflect(normalize(viewPos), Nv); // mirror dir in view space
    if (Rv.z >= -0.02) { rt_ssr = vec4(0.0); return; } // grazing / toward camera

    // Linear march in view space with a thickness tolerance, then binary refine.
    // (A Hi-Z coarse pass can replace the linear march later without changing
    // the hit/refine/blend logic below.)
    const int STEPS = 48;
    const float THICKNESS = 0.15;
    const float tMax = ubo.params.w;
    float t = 0.05;
    float prevT = t;
    float hitT = -1.0;
    vec2 hitUV = vec2(0.0);
    for (int i = 0; i < STEPS; ++i) {
        float tt = t + (tMax - t) * float(i) / float(STEPS - 1);
        vec3 p = viewPos + Rv * tt;
        vec2 uv;
        if (!project(p, uv)) break;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break; // off-screen
        float diff = p.z - sceneViewZ(uv, texture(sampler2D(tex_depth, samp_depth), uv).r);
        if (diff > 0.0 && diff < THICKNESS) { hitT = tt; hitUV = uv; break; }
        prevT = tt;
    }
    if (hitT < 0.0) { rt_ssr = vec4(0.0); return; }

    // Binary-search refinement between the last free sample and the hit.
    float lo = prevT, hi = hitT;
    vec2 uv = hitUV;
    for (int i = 0; i < 6; ++i) {
        float mid = (lo + hi) * 0.5;
        vec3 p = viewPos + Rv * mid;
        vec2 muv;
        if (!project(p, muv)) break;
        float diff = p.z - sceneViewZ(muv, texture(sampler2D(tex_depth, samp_depth), muv).r);
        if (diff > 0.0) { hi = mid; uv = muv; } else { lo = mid; }
    }

    // Confidence: screen-edge fade, ray-length fade, grazing-angle fade.
    float edge = smoothstep(0.0, 0.05, min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)));
    float rayFade = clamp(1.0 - hi / tMax, 0.0, 1.0);
    vec3 V = normalize(ubo.camPos.xyz - worldPos);
    float grazing = clamp(dot(N, V), 0.0, 1.0);
    float confidence = edge * rayFade * grazing;

    vec3 albedo = texture(sampler2D(tex_albedo, samp_albedo), v_uv).rgb;
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 refl = texture(sampler2D(tex_hdr, samp_hdr), uv).rgb;
    rt_ssr = vec4(refl * F0 * confidence, 1.0);
}
