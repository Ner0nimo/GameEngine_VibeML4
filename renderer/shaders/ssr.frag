#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 rt_ssr; // rgba16f: rgb = reflected scene color, a = hit confidence

// NVRHI Vulkan binding convention: SRV at binding=slot, sampler at 128+slot, CB at 256+slot.
layout(set = 0, binding = 0) uniform texture2D tex_hdr_prev; // last frame's lit HDR (reflection source)
layout(set = 0, binding = 128) uniform sampler samp_hdr_prev;
layout(set = 0, binding = 1) uniform texture2D tex_depth;    // gbuffer depth
layout(set = 0, binding = 129) uniform sampler samp_depth;
layout(set = 0, binding = 2) uniform texture2D tex_normal;
layout(set = 0, binding = 130) uniform sampler samp_normal;
layout(set = 0, binding = 3) uniform texture2D tex_orm;
layout(set = 0, binding = 131) uniform sampler samp_orm;
layout(set = 0, binding = 4) uniform texture2D tex_worldPos;
layout(set = 0, binding = 132) uniform sampler samp_worldPos;
layout(set = 0, binding = 261) uniform UniformBuffer {
    vec4 params;    // x = enabled, y = strength, z = max roughness, w = max ray distance (view units)
    vec4 camPos;    // xyz = camera position, w = 1
    mat4 camView;
    mat4 camProj;
    mat4 camInvProj;
} ubo;

// View-space z of the scene point stored at depth d and uv.
float sceneViewZ(vec2 uv, float d) {
    vec4 sc = ubo.camInvProj * vec4(uv * 2.0 - 1.0, d, 1.0);
    return sc.z / sc.w;
}

// Project a view-space point to screen uv (false if behind the camera).
bool project(vec3 p, out vec2 uv) {
    vec4 clip = ubo.camProj * vec4(p, 1.0);
    if (clip.w <= 0.0) return false;
    uv = clip.xy / clip.w * 0.5 + 0.5;
    return true;
}

// Depth diff of a view-space ray point vs the scene surface at its screen uv.
float rayDiff(vec3 p, vec2 uv) {
    return p.z - sceneViewZ(uv, texture(sampler2D(tex_depth, samp_depth), uv).r);
}

void main() {
    float depth = texture(sampler2D(tex_depth, samp_depth), v_uv).r;
    if (depth >= 1.0 || ubo.params.x < 0.5) { rt_ssr = vec4(0.0); return; } // sky / disabled
    float roughness = clamp(texture(sampler2D(tex_orm, samp_orm), v_uv).g, 0.04, 1.0);
    if (roughness > ubo.params.z) { rt_ssr = vec4(0.0); return; }

    vec3 worldPos = texture(sampler2D(tex_worldPos, samp_worldPos), v_uv).xyz;
    vec3 N = normalize(texture(sampler2D(tex_normal, samp_normal), v_uv).xyz * 2.0 - 1.0);
    vec3 viewPos = vec3(ubo.camView * vec4(worldPos, 1.0));
    vec3 Nv = normalize(mat3(ubo.camView) * N);
    vec3 Rv = reflect(normalize(viewPos), Nv); // mirror dir in view space
    if (Rv.z >= -0.02) { rt_ssr = vec4(0.0); return; } // grazing / toward camera

    // DEBUG: encode viewPos.z, sceneZ at v_uv, Rv.z into rgb
    {
        float sz = sceneViewZ(v_uv, depth);
        rt_ssr = vec4(clamp(viewPos.z * 0.05 + 0.5, 0.0, 1.0),
                      clamp(sz * 0.05 + 0.5, 0.0, 1.0),
                      clamp(Rv.z + 0.5, 0.0, 1.0), 0.0);
        return;
    }
    vec3 V = normalize(ubo.camPos.xyz - worldPos);
    float NdotV = clamp(dot(N, V), 0.0, 1.0);
    // Start offset grows at grazing angles so the ray doesn't immediately
    // re-hit its own surface.
    const float t0 = 0.05 + (1.0 - NdotV) * 0.5;
    const float tMax = min(ubo.params.w, 25.0);
    const int STEPS = 128;
    const float THICKNESS = 0.15;

    // March and register a hit on a SIGN CHANGE (in-front -> behind), so a
    // crossing between two samples is not tunneled through. The thickness
    // check then rejects surfaces that are too far behind the ray at the
    // crossing (thin geometry). Binary refine converges to the crossing.
    // (A Hi-Z coarse pass can replace this linear march later without
    // changing the crossing/refine/blend logic.)
    float prevT = t0;
    float prevDiff = 1e9;
    float maxDiff = -1e9; // DEBUG
    float hitT = -1.0;
    vec2 hitUV = vec2(0.0);
    for (int i = 0; i < STEPS; ++i) {
        float tt = t0 + (tMax - t0) * float(i) / float(STEPS - 1);
        vec3 p = viewPos + Rv * tt;
        vec2 uv;
        if (!project(p, uv)) break;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break; // off-screen
        float diff = rayDiff(p, uv);
        maxDiff = max(maxDiff, diff); // DEBUG
        // i > 0: the first sample sits on the source surface itself (self-crossing).
        if (i > 0 && prevDiff > 0.0 && diff <= 0.0) {
            float lo = prevT, hi = tt;
            vec2 huv = uv;
            for (int r = 0; r < 6; ++r) {
                float mid = (lo + hi) * 0.5;
                vec3 pm = viewPos + Rv * mid;
                vec2 muv;
                if (!project(pm, muv)) break;
                if (rayDiff(pm, muv) > 0.0) lo = mid; else { hi = mid; huv = muv; }
            }
            if (rayDiff(viewPos + Rv * hi, huv) > -THICKNESS) { hitT = hi; hitUV = huv; break; }
            // Rejected (surface too far behind the ray): keep marching.
        }
        prevT = tt;
        prevDiff = diff;
    }
    if (hitT < 0.0) { rt_ssr = vec4(0.0, 0.0, 0.0, clamp(maxDiff * 0.5, 0.0, 1.0)); return; } // DEBUG: no hit, encode maxDiff

    // Confidence: screen-edge fade, ray-length fade, grazing-angle fade.
    float edge = smoothstep(0.0, 0.05, min(min(hitUV.x, 1.0 - hitUV.x), min(hitUV.y, 1.0 - hitUV.y)));
    float rayFade = clamp(1.0 - hitT / tMax, 0.0, 1.0);
    float confidence = edge * rayFade * NdotV;

    vec3 refl = texture(sampler2D(tex_hdr_prev, samp_hdr_prev), hitUV).rgb;
    rt_ssr = vec4(refl, confidence);
}
