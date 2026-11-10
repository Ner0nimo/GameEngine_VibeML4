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

layout(set = 1, binding = 256) uniform UniformBuffer {
    vec3 camPos;
    float _pad0;
    vec3 lightDir;       // direction the light travels (e.g. (0,-1,0)); L = -lightDir
    float lightIntensity;
    vec3 lightColor;
    float _pad1;
    mat4 lightViewProj;
    vec2 shadowMapInvSize;  // 1 / shadow map size
    vec2 shadowDepthRange;  // x = znear, y = zfar - znear of the light ortho frustum
    float shadowBias;
    float ambientStrength;
    float envMipLevels;       // mip count of tex_prefilter
    float shadowSearchTexels; // PCSS blocker-search radius, in texels
    float shadowMaxPenumbra;  // PCSS max filter radius, in texels
    float shadowLightSize;    // world-space light size driving the penumbra width
    float shadowBoxWidth;     // world width of the light ortho box (for texel size)
    float _pad2;
    vec4 clearColor;
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

// Percentage-closer soft shadows: blocker search -> penumbra width from
// similar triangles -> PCF kernel scaled to the penumbra. Contact points get
// hard shadows, distant occluders get wide soft ones.
float shadowPCSS(vec4 worldPos, vec3 N, vec3 L) {
    vec4 sc = ubo.lightViewProj * worldPos;
    vec3 proj = sc.xyz / sc.w;
    if (proj.z > 1.0 || proj.z < 0.0 ||
        proj.x < -1.0 || proj.x > 1.0 || proj.y < -1.0 || proj.y > 1.0) return 1.0;
    vec2 uv = proj.xy * 0.5 + 0.5;
    float bias = ubo.shadowBias * (1.0 - max(dot(N, L), 0.0)) + ubo.shadowBias * 0.5;
    float receiver = proj.z - bias;

    // 1) Blocker search: average depth of occluders in a fixed neighborhood.
    float searchUv = ubo.shadowSearchTexels * ubo.shadowMapInvSize.x;
    float blockerSum = 0.0;
    float blockers = 0.0;
    for (int x = -2; x <= 2; ++x)
        for (int y = -2; y <= 2; ++y) {
            float d = texture(sampler2D(tex_shadow, samp_shadow),
                              uv + vec2(float(x), float(y)) * searchUv).r;
            if (d < receiver) { blockerSum += d; blockers += 1.0; }
        }
    if (blockers < 1.0) return 1.0; // no occluder nearby -> fully lit
    float avgBlocker = blockerSum / blockers;

    // 2) Penumbra width in shadow-map texels. Ortho depth is linear in
    // view distance, so ndc deltas convert with the frustum depth span.
    float zn = ubo.shadowDepthRange.x;
    float zspan = ubo.shadowDepthRange.y;
    float blockerDist = avgBlocker * zspan + zn;
    float worldDelta = (receiver - avgBlocker) * zspan;
    float penumbraWorld = worldDelta * ubo.shadowLightSize / max(blockerDist, 1e-3);
    float worldPerTexel = ubo.shadowBoxWidth * ubo.shadowMapInvSize.x;
    float radius = clamp(penumbraWorld / max(worldPerTexel, 1e-6), 1.0, ubo.shadowMaxPenumbra);

    // 3) PCF over a 7x7 kernel scaled to the penumbra radius.
    float sum = 0.0;
    const int R = 3;
    for (int x = -R; x <= R; ++x)
        for (int y = -R; y <= R; ++y) {
            vec2 suv = uv + vec2(float(x), float(y)) * ubo.shadowMapInvSize * (radius / float(R));
            float d = texture(sampler2D(tex_shadow, samp_shadow), suv).r;
            sum += (d >= receiver) ? 1.0 : 0.0;
        }
    return sum / float((2 * R + 1) * (2 * R + 1));
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
    vec3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 0.0);

    vec3 F0 = mix(vec3(0.04), albedo.rgb, metallic);

    // Directional light with PCSS soft shadow.
    float shadow = shadowPCSS(vec4(worldPos, 1.0), N, L);
    float NdotL = max(dot(N, L), 0.0);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 specular = (NDF * G * F) / max(4.0 * NdotV * NdotL, 1e-7);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 direct = (kD * albedo.rgb / PI + specular) * ubo.lightColor * ubo.lightIntensity * NdotL * shadow;

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
    vec3 ambient = (ambientDiffuse + ambientSpec) * ao * ssao * ubo.ambientStrength;

    vec3 color = ambient + direct + emissive;
    rt_hdr = vec4(color, 1.0);
}
