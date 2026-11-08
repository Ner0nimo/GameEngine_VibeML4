#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 rt_hdr; // rgba16f, linear HDR

layout(set = 0, binding = 0) uniform sampler2D tex_albedo;
layout(set = 0, binding = 1) uniform sampler2D tex_normal;
layout(set = 0, binding = 2) uniform sampler2D tex_orm;
layout(set = 0, binding = 3) uniform sampler2D tex_emissive;
layout(set = 0, binding = 4) uniform sampler2D tex_worldPos;
layout(set = 0, binding = 5) uniform sampler2D tex_ao;
layout(set = 0, binding = 6) uniform sampler2DShadow tex_shadow;
layout(set = 0, binding = 7) uniform samplerCube tex_irradiance;

layout(set = 1, binding = 0) uniform UniformBuffer {
    vec3 camPos;
    float _pad0;
    vec3 lightDir;       // direction the light travels (e.g. (0,-1,0)); L = -lightDir
    float lightIntensity;
    vec3 lightColor;
    float _pad1;
    mat4 lightViewProj;
    vec2 shadowMapInvSize;
    float shadowBias;
    float ambientStrength;
    vec4 clearColor;
} ubo;

const float PI = 3.14159265359;

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
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

float shadowPCF(vec4 worldPos, vec3 N, vec3 L) {
    vec4 sc = ubo.lightViewProj * worldPos;
    vec3 proj = sc.xyz / sc.w;
    if (proj.z > 1.0 || proj.z < 0.0 ||
        proj.x < -1.0 || proj.x > 1.0 || proj.y < -1.0 || proj.y > 1.0) return 1.0;
    vec2 uv = proj.xy * 0.5 + 0.5;
    float bias = ubo.shadowBias * (1.0 - max(dot(N, L), 0.0)) + ubo.shadowBias * 0.5;
    float sum = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            sum += texture(tex_shadow, vec3(uv + vec2(float(x), float(y)) * ubo.shadowMapInvSize * 2.0,
                                             proj.z - bias));
    return sum / 9.0; // 3x3 PCF -> soft shadows
}

void main() {
    vec4 albedo = texture(tex_albedo, v_uv);
    if (albedo.a < 0.01) { // sky: output clear color
        rt_hdr = vec4(ubo.clearColor.rgb, 1.0);
        return;
    }
    vec3 worldPos = texture(tex_worldPos, v_uv).xyz;
    vec3 N = normalize(texture(tex_normal, v_uv).xyz * 2.0 - 1.0);
    vec3 orm = texture(tex_orm, v_uv).rgb;
    float ao = orm.r;
    float roughness = clamp(orm.g, 0.04, 1.0);
    float metallic = clamp(orm.b, 0.0, 1.0);
    vec3 emissive = texture(tex_emissive, v_uv).rgb;
    float ssao = texture(tex_ao, v_uv).r;

    vec3 V = normalize(ubo.camPos - worldPos);
    vec3 L = normalize(-ubo.lightDir);
    vec3 H = normalize(V + L);

    vec3 F0 = mix(vec3(0.04), albedo.rgb, metallic);

    // Directional light with soft shadow.
    float shadow = shadowPCF(vec4(worldPos, 1.0), N, L);
    float NdotL = max(dot(N, L), 0.0);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 specular = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * NdotL, 1e-7);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 direct = (kD * albedo.rgb / PI + specular) * ubo.lightColor * ubo.lightIntensity * NdotL * shadow;

    // IBL ambient: irradiance cubemap (diffuse) + simple specular approximation.
    vec3 irradiance = texture(tex_irradiance, N).rgb;
    vec3 ambientDiffuse = irradiance * albedo.rgb * (1.0 - metallic);
    vec3 R = reflect(-V, N);
    vec3 ambientSpec = texture(tex_irradiance, R).rgb * mix(vec3(0.04), albedo.rgb, metallic) * 0.5;
    vec3 ambient = (ambientDiffuse + ambientSpec) * ao * ssao * ubo.ambientStrength;

    vec3 color = ambient + direct + emissive;
    rt_hdr = vec4(color, 1.0);
}
