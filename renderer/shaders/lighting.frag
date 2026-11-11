#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 rt_hdr; // rgba16f, linear HDR

// NVRHI Vulkan binding convention: SRV at binding=slot, sampler at 128+slot, CB at 256+slot.
layout(set = 0, binding = 0) uniform texture2D tex_albedo;
layout(set = 0, binding = 128) uniform sampler samp_albedo;
layout(set = 0, binding = 1) uniform texture2D tex_normal;
layout(set = 0, binding = 129) uniform sampler samp_normal;
layout(set = 0, binding = 2) uniform texture2D tex_orm;
layout(set = 0, binding = 130) uniform sampler samp_orm;
layout(set = 0, binding = 3) uniform texture2D tex_emissive;
layout(set = 0, binding = 131) uniform sampler samp_emissive;
layout(set = 0, binding = 4) uniform texture2D tex_worldPos;
layout(set = 0, binding = 132) uniform sampler samp_worldPos;
layout(set = 0, binding = 5) uniform texture2D tex_ao;
layout(set = 0, binding = 133) uniform sampler samp_ao;
layout(set = 0, binding = 6) uniform texture2D tex_shadow; // depth (r), manual PCF compare
layout(set = 0, binding = 134) uniform sampler samp_shadow;
layout(set = 0, binding = 7) uniform textureCube tex_irradiance; // diffuse IBL
layout(set = 0, binding = 135) uniform sampler samp_irradiance;
layout(set = 0, binding = 8) uniform textureCube tex_prefilter; // mipmapped radiance (specular IBL)
layout(set = 0, binding = 136) uniform sampler samp_prefilter;
layout(set = 0, binding = 9) uniform texture2D tex_brdf; // RG LUT: (scale, bias) by (NdotV, roughness)
layout(set = 0, binding = 137) uniform sampler samp_brdf;
layout(set = 0, binding = 10) uniform texture2D tex_ssr; // rgba16f: rgb = reflected color, a = confidence
layout(set = 0, binding = 138) uniform sampler samp_ssr;


layout(set = 1, binding = 256) uniform UniformBuffer {
    vec3 camPos;
    float _pad0;
    vec3 lightDir;       // direction the light travels (e.g. (0,-1,0)); L = -lightDir
    float lightIntensity;
    vec3 lightColor;
    float _pad1;
    mat4 lightViewProj;    // cascade 0 matrix (legacy single-map path)
    vec2 shadowMapInvSize; // 1 / slice size (both axes)
    float shadowBias;
    float ambientStrength;
    float envMipLevels;       // mip count of tex_prefilter
    float shadowSearchTexels; // PCSS blocker-search radius, in texels
    float shadowMaxPenumbra;  // PCSS max filter radius, in texels
    float shadowLightSize;    // world-space light size driving the penumbra width
    float _pad2;
    vec3 ambientColor;        // flat ambient light color (separate from IBL)
    float _padAmb;
    vec4 clearColor;
    // Local lights (unshadowed), fixed-capacity vec4 arrays.
    vec4 pointPosRange[8];   // xyz = world pos, w = range
    vec4 pointColorInt[8];   // rgb = color, w = intensity
    vec4 spotPos[4];         // xyz = world pos
    vec4 spotDirCone[4];     // xyz = direction, w = cos(outer angle)
    vec4 spotColorInt[4];    // rgb = color, w = intensity
    vec4 spotInnerRange[4];  // x = cos(inner angle), y = range
    vec2 lightCounts;        // x = point count, y = spot count
    float _pad3, _pad4;
    mat4 camView;            // camera view matrix (cascade selection)
    mat4 cascadeViewProj[4]; // per-cascade light view-proj (atlas slices)
    vec4 cascadeSplits;      // xyz = split distances 1..3, w = far (view space)
    vec4 shadowCascade;      // x = active cascade count (1 = single-map fallback)
    vec4 cascadeFrustum[4];  // per cascade: x = znear, y = zspan, z = box width
} ubo;

const float PI = 3.14159265359;

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}
float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom + 1e-7);
}
float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return (NdotV / (NdotV * (1.0 - k) + k + 1e-7)) * (NdotL / (NdotL * (1.0 - k) + k + 1e-7));
}

// Cook-Torrance direct term (diffuse + specular) * NdotL for one light
// direction L, without color/intensity/attenuation — callers multiply those.
vec3 directTerm(vec3 N, vec3 V, vec3 L, vec3 albedo, float roughness, float metallic, vec3 F0) {
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return vec3(0.0);
    vec3 H = normalize(V + L);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 specular = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * NdotL, 1e-7);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    return (kD * albedo / PI + specular) * NdotL;
}

// Smooth inverse-square-ish attenuation with a cutoff at the light range.
float rangeAttenuation(float dist, float range) {
    float x = clamp(1.0 - pow(dist / max(range, 1e-4), 4.0), 0.0, 1.0);
    return x * x / (dist * dist + 1.0);
}

// PCSS inside one cascade. uvLocal is the slice-local [0,1] shadow UV and c
// the cascade index; blocker search and PCF run in the selected cascade's
// coordinate system so softness doesn't pop at cascade switches.
// The shadow map is a vertical atlas: slice c occupies v in [c/4, (c+1)/4].
// Percentage-closer soft shadows: blocker search -> penumbra width from
// similar triangles -> PCF kernel scaled to the penumbra.
float pcssCascade(int c, vec4 worldPos, vec3 N, vec3 L) {
    vec4 sc = ubo.cascadeViewProj[c] * worldPos;
    vec3 proj = sc.xyz / sc.w;
    if (proj.z > 1.0 || proj.z < 0.0 ||
        proj.x < -1.0 || proj.x > 1.0 || proj.y < -1.0 || proj.y > 1.0) return 1.0;
    vec2 uvLocal = proj.xy * 0.5 + 0.5;
    float bias = ubo.shadowBias * (1.0 - max(dot(N, L), 0.0)) + ubo.shadowBias * 0.5;
    float receiver = proj.z - bias;
    vec4 fr = ubo.cascadeFrustum[c]; // x = znear, y = zspan, z = box width
    float inv = ubo.shadowMapInvSize.x;

    // 1) Blocker search: average depth of occluders in a fixed neighborhood.
    float searchUv = ubo.shadowSearchTexels * inv;
    float blockerSum = 0.0;
    float blockers = 0.0;
    for (int x = -2; x <= 2; ++x)
        for (int y = -2; y <= 2; ++y) {
            vec2 luv = uvLocal + vec2(float(x), float(y)) * searchUv;
            vec2 uv = vec2(luv.x, (float(c) + luv.y) / 4.0);
            float d = texture(sampler2D(tex_shadow, samp_shadow), uv).r;
            if (d < receiver) { blockerSum += d; blockers += 1.0; }
        }
    if (blockers < 1.0) return 1.0; // no occluder nearby -> fully lit
    float avgBlocker = blockerSum / blockers;

    // 2) Penumbra width in texels. Ortho depth is linear in view distance, so
    // ndc deltas convert with this cascade's depth span.
    float blockerDist = avgBlocker * fr.y + fr.x;
    float worldDelta = (receiver - avgBlocker) * fr.y;
    float penumbraWorld = worldDelta * ubo.shadowLightSize / max(blockerDist, 1e-3);
    float worldPerTexel = fr.z * inv;
    float radius = clamp(penumbraWorld / max(worldPerTexel, 1e-6), 1.0, ubo.shadowMaxPenumbra);

    // 3) PCF over a 7x7 kernel scaled to the penumbra radius.
    float sum = 0.0;
    const int R = 3;
    for (int x = -R; x <= R; ++x)
        for (int y = -R; y <= R; ++y) {
            vec2 luv = uvLocal + vec2(float(x), float(y)) * inv * (radius / float(R));
            vec2 uv = vec2(luv.x, (float(c) + luv.y) / 4.0);
            float d = texture(sampler2D(tex_shadow, samp_shadow), uv).r;
            sum += (d >= receiver) ? 1.0 : 0.0;
        }
    return sum / float((2 * R + 1) * (2 * R + 1));
}

// View-space distance of cascade split i (i in 0..3); split 3 is the far plane.
float cascadeSplitAt(int i) {
    return i < 3 ? ubo.cascadeSplits[i] : ubo.cascadeSplits.w;
}

int selectCascade(float viewZ, int count) {
    for (int i = 0; i < 4; ++i)
        if (i >= count - 1 || viewZ <= cascadeSplitAt(i)) return i;
    return count - 1;
}

// CSM shadow: pick the cascade by view depth, PCSS inside it, and blend with
// the next cascade over the last 20% of the current one (all boundaries).
float shadowCSM(vec4 worldPos, vec3 N, vec3 L) {
    float viewZ = -vec4(ubo.camView * worldPos).z;
    int count = max(int(ubo.shadowCascade.x + 0.5), 1);
    int c = selectCascade(viewZ, count);
    float s = pcssCascade(c, worldPos, N, L);
    const float band = 0.2;
    if (c + 1 < count) {
        float split = cascadeSplitAt(c);
        float w = clamp((viewZ - split * (1.0 - band)) / max(split * band, 1e-4), 0.0, 1.0);
        if (w > 0.0) s = mix(s, pcssCascade(c + 1, worldPos, N, L), w);
    }
    return s;
}

void main() {
    vec4 albedo = texture(sampler2D(tex_albedo, samp_albedo), v_uv);
    if (albedo.a < 0.01) { // sky: output clear color
        rt_hdr = vec4(ubo.clearColor.rgb, 1.0);
        return;
    }
    vec3 worldPos = texture(sampler2D(tex_worldPos, samp_worldPos), v_uv).xyz;
    vec3 N = normalize(texture(sampler2D(tex_normal, samp_normal), v_uv).xyz * 2.0 - 1.0);
    vec3 orm = texture(sampler2D(tex_orm, samp_orm), v_uv).rgb;
    float ao = orm.r;
    float roughness = clamp(orm.g, 0.04, 1.0);
    float metallic = clamp(orm.b, 0.0, 1.0);
    vec3 emissive = texture(sampler2D(tex_emissive, samp_emissive), v_uv).rgb;
    float ssao = texture(sampler2D(tex_ao, samp_ao), v_uv).r;

    vec3 V = normalize(ubo.camPos - worldPos);
    vec3 L = normalize(-ubo.lightDir);
    float NdotV = max(dot(N, V), 0.0);

    vec3 F0 = mix(vec3(0.04), albedo.rgb, metallic);

    // Directional light with CSM + PCSS soft shadow.
    float shadow = shadowCSM(vec4(worldPos, 1.0), N, L);
    vec3 direct = directTerm(N, V, L, albedo.rgb, roughness, metallic, F0)
                * ubo.lightColor * ubo.lightIntensity * shadow;

    // Point lights (unshadowed).
    for (int i = 0; i < 8; ++i) {
        if (float(i) >= ubo.lightCounts.x) break;
        vec3 toL = ubo.pointPosRange[i].xyz - worldPos;
        float dist = length(toL);
        vec3 Li = toL / max(dist, 1e-4);
        float att = rangeAttenuation(dist, ubo.pointPosRange[i].w);
        direct += directTerm(N, V, Li, albedo.rgb, roughness, metallic, F0)
                * ubo.pointColorInt[i].rgb * ubo.pointColorInt[i].w * att;
    }

    // Spot lights (unshadowed): cone penumbra between inner and outer angle.
    for (int i = 0; i < 4; ++i) {
        if (float(i) >= ubo.lightCounts.y) break;
        vec3 toL = ubo.spotPos[i].xyz - worldPos;
        float dist = length(toL);
        vec3 Li = toL / max(dist, 1e-4);
        float att = rangeAttenuation(dist, ubo.spotInnerRange[i].y);
        float cosOuter = ubo.spotDirCone[i].w;
        float cosInner = ubo.spotInnerRange[i].x;
        float cone = clamp((dot(-Li, ubo.spotDirCone[i].xyz) - cosOuter)
                           / max(cosInner - cosOuter, 1e-4), 0.0, 1.0);
        direct += directTerm(N, V, Li, albedo.rgb, roughness, metallic, F0)
                * ubo.spotColorInt[i].rgb * ubo.spotColorInt[i].w * att * cone;
    }

    // IBL ambient: irradiance cubemap (diffuse) + split-sum specular
    // (prefiltered radiance cubemap by roughness, modulated by the BRDF LUT).
    vec3 irradiance = texture(samplerCube(tex_irradiance, samp_irradiance), N).rgb;
    vec3 ambientDiffuse = irradiance * albedo.rgb * (1.0 - metallic);
    vec3 R = reflect(-V, N);
    vec3 Famb = fresnelSchlickRoughness(NdotV, F0, roughness);
    float maxLod = max(ubo.envMipLevels - 1.0, 0.0);
    vec3 prefiltered = textureLod(samplerCube(tex_prefilter, samp_prefilter), R, roughness * maxLod).rgb;
    vec2 brdf = texture(sampler2D(tex_brdf, samp_brdf), vec2(NdotV, roughness)).rg;
    vec3 ambientSpec = prefiltered * (Famb * brdf.x + brdf.y);
    // SSR replaces the IBL specular where a confident hit exists (blend by
    // confidence, never add both), using the same BRDF/Fresnel weighting.
    vec4 ssr = texture(sampler2D(tex_ssr, samp_ssr), v_uv);
    float ssrConf = clamp(ssr.a, 0.0, 1.0); // alpha already includes strength (set in ssr.frag)
    vec3 ssrSpec = ssr.rgb * (Famb * brdf.x + brdf.y);
    ambientSpec = mix(ambientSpec, ssrSpec, ssrConf);
    vec3 ambient = (ambientDiffuse + ambientSpec) * ao * ssao * ubo.ambientStrength;
    // Flat ambient term from scene settings (kept separate from the IBL
    // ambient): ambientColor already includes the intensity, and
    // ambientStrength only scales the IBL term above.
    ambient += ubo.ambientColor * albedo.rgb * (1.0 - metallic);

    vec3 color = ambient + direct + emissive;
    rt_hdr = vec4(color, 1.0);
}
