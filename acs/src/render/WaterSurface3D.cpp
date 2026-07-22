// SPDX-License-Identifier: Apache-2.0
#include "render/WaterSurface3D.h"

#include "asset/MeshAsset.h"
#include "foundation/Log.h"
#include "foundation/Move.h"
#include "memory/Allocator.h"
#include "memory/Memory.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiDevice.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/IRhiTexture.h"

#include <cmath>
#include <limits>

namespace acs {

namespace {

constexpr f32 kTwoPi = 6.28318530718f;

template<typename T>
constexpr usize ConstantBufferSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

struct FWaterFrameCb {
    FMat4 view_projection;
    FMat4 inverse_view_projection;
    FMat4 light_view_projection;
    FVec4 camera_time;
    FVec4 screen_params;
    FVec4 sun_direction;
    FVec4 sun_color;
    FVec4 shallow_roughness;
    FVec4 deep_normal;
    FVec4 absorption_refraction;
    FVec4 scattering_phase;
    FVec4 foam_depth;
    FVec4 wave_params;
    FVec4 flow_params;
    FVec4 surface_misc;
    FVec4 shadow_params;
    FVec4 ripple_a[FWaterSurface3D::kMaxRipples];
    FVec4 ripple_b[FWaterSurface3D::kMaxRipples];
};

struct FWaterObjectCb {
    FMat4 model;
};

const char* kWaterSurface3DHlsl = R"(
#pragma pack_matrix(row_major)

static const int kMaxRipples = 64;
static const float kPi = 3.14159265359;

cbuffer WaterFrame : register(b0) {
    float4x4 view_projection;
    float4x4 inverse_view_projection;
    float4x4 light_view_projection;
    float4 camera_time;          // xyz=camera position, w=time
    float4 screen_params;        // xy=1/size, zw=size
    float4 sun_direction;        // xyz=surface -> sun
    float4 sun_color;            // rgb=HDR radiance
    float4 shallow_roughness;    // rgb=shallow color, w=roughness
    float4 deep_normal;          // rgb=deep color, w=normal strength
    float4 absorption_refraction;// rgb=Beer-Lambert absorption, w=refraction strength
    float4 scattering_phase;     // rgb=volume scattering, w=HG anisotropy
    float4 foam_depth;           // rgb=foam color, w=foam intensity
    float4 wave_params;          // x=amplitude, y=scale, z=speed, w=normal tiling
    float4 flow_params;          // xy=flow direction, z=ripple wavelength, w=ripple count
    float4 surface_misc;         // x=optical depth, y=scene color, z=depth, w=reflection
    float4 shadow_params;        // x=enabled, y=bias, z=texel size, w=PCF radius
    float4 ripple_a[kMaxRipples];// xy=center XZ, z=current radius, w=current amplitude
    float4 ripple_b[kMaxRipples];// xy=direction XZ, z=anisotropy, w=remaining life
};

cbuffer WaterObject : register(b1) {
    float4x4 model;
};

Texture2D water_normal : register(t0);
SamplerState water_normal_sampler : register(s0);
Texture2D scene_color : register(t1);
SamplerState scene_color_sampler : register(s1);
Texture2D scene_depth : register(t2);
SamplerState scene_depth_sampler : register(s2);
Texture2D screen_reflection : register(t3);
SamplerState screen_reflection_sampler : register(s3);
Texture2D shadow_map : register(t4);
SamplerState shadow_map_sampler : register(s4);

struct VSIn {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VSOut {
    float4 position : SV_POSITION;
    float3 world_position : POSITION;
    float3 world_normal : NORMAL;
    float2 uv : TEXCOORD0;
    float ripple_energy : TEXCOORD1;
    float wave_height : TEXCOORD2;
};

float2 Rotate2(float2 p, float angle) {
    float s = sin(angle);
    float c = cos(angle);
    return float2(c * p.x - s * p.y, s * p.x + c * p.y);
}

void EvaluateAmbientWaves(float2 p, float time,
                          out float height, out float2 gradient) {
    float2 flow = normalize(flow_params.xy + float2(1e-5, 0.0));
    float2 side = float2(-flow.y, flow.x);
    float2 directions[4] = {
        normalize(flow + side * 0.18),
        normalize(flow - side * 0.83),
        normalize(-flow + side * 0.47),
        normalize(flow + side * 1.71)
    };
    float frequencies[4] = { 0.72, 1.37, 2.51, 4.73 };
    float amplitudes[4]  = { 0.52, 0.27, 0.145, 0.085 };
    float velocities[4]  = { 0.87, 1.19, 0.63, 1.43 };
    float phases[4]      = { 0.20, 1.73, 3.11, 5.02 };

    height = 0.0;
    gradient = 0.0;
    float spatial_scale = max(wave_params.y, 0.01);
    float animation_speed = wave_params.z;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        float k = frequencies[i] * spatial_scale;
        float phase = dot(p, directions[i]) * k
                    - time * animation_speed * velocities[i] + phases[i];
        float amplitude = amplitudes[i] * wave_params.x;
        height += sin(phase) * amplitude;
        gradient += cos(phase) * amplitude * k * directions[i];
    }
}

void EvaluateRipples(float2 p, out float height,
                     out float2 gradient, out float energy) {
    height = 0.0;
    gradient = 0.0;
    energy = 0.0;
    const float wavelength = max(flow_params.z, 0.025);
    const float wave_number = 2.0 * kPi / wavelength;
    // A broad packet carries several diminishing crests instead of reading as
    // one solid, expanding wall.
    const float sigma = wavelength * 1.10;
    const float inv_sigma2 = 1.0 / max(sigma * sigma, 1e-5);
    const int count = clamp((int)flow_params.w, 0, kMaxRipples);

    [loop]
    for (int i = 0; i < kMaxRipples; ++i) {
        if (i >= count) break;
        float2 direction = normalize(ripple_b[i].xy + float2(1e-5, 0.0));
        float2 side = float2(-direction.y, direction.x);
        float2 delta = p - ripple_a[i].xy;
        float along = dot(delta, direction);
        float across = dot(delta, side);
        float anisotropy = max(ripple_b[i].z, 1.0);

        float2 metric = float2(along / anisotropy, across);
        float distance_to_center = max(length(metric), 1e-4);
        float radial = distance_to_center - ripple_a[i].z;
        // Outside 3.75 sigma the Gaussian packet is below 8e-7. Rejecting it
        // before exp/sin/cos preserves the visible result while turning large
        // mostly-unaffected meshes from 64 transcendental evaluations per
        // vertex into cheap distance checks.
        if (abs(radial) > sigma * 3.75) continue;
        float envelope = exp(-radial * radial * inv_sigma2);

        // Directional wake events are elongated and biased behind the contact.
        float wake_weight = saturate((anisotropy - 1.0) * 0.72);
        float rear_mask = smoothstep(ripple_a[i].z * 0.45,
                                     -ripple_a[i].z * 0.75, along);
        envelope *= lerp(1.0, 0.24 + rear_mask * 0.76, wake_weight);

        float phase = radial * wave_number;
        float amplitude = ripple_a[i].w;
        float wave = amplitude * cos(phase) * envelope;
        float derivative = amplitude * envelope
                         * (-wave_number * sin(phase)
                            - 2.0 * radial * inv_sigma2 * cos(phase));

        float2 local_gradient = metric / distance_to_center;
        local_gradient.x /= anisotropy;
        float2 world_gradient =
            direction * local_gradient.x + side * local_gradient.y;

        height += wave;
        gradient += derivative * world_gradient;
        // Foam/roughness energy follows positive crests only.  Using the whole
        // envelope produces an opaque white doughnut around every impact.
        energy += max(wave, 0.0)
                * lerp(0.68, 1.0, saturate(ripple_b[i].w));
    }
}

VSOut VSMain(VSIn input) {
    VSOut output;
    float4 world = mul(float4(input.position, 1.0), model);

    float ambient_height;
    float2 ambient_gradient;
    EvaluateAmbientWaves(world.xz, camera_time.w,
                         ambient_height, ambient_gradient);

    float ripple_height;
    float2 ripple_gradient;
    float ripple_energy;
    EvaluateRipples(world.xz, ripple_height, ripple_gradient, ripple_energy);

    float2 total_gradient = ambient_gradient + ripple_gradient;
    world.y += ambient_height + ripple_height;
    output.world_position = world.xyz;
    output.world_normal = normalize(float3(-total_gradient.x, 1.0,
                                            -total_gradient.y));
    output.position = mul(world, view_projection);
    output.uv = input.uv;
    output.ripple_energy = ripple_energy;
    output.wave_height = ambient_height + ripple_height;
    return output;
}

float4 SampleNormalLayer(float2 world_xz, float angle, float scale,
                         float2 scroll, out float detail_weight) {
    float2 uv = Rotate2(world_xz, angle) * scale + scroll;
    // The generated map contains detail up to roughly 16 cycles per tile. Fade
    // a layer before that frequency exceeds the pixel footprint's Nyquist
    // limit. This avoids distant shimmer even on backends where the generated
    // texture has only a base mip.
    float footprint = max(length(ddx(uv)), length(ddy(uv)));
    float cycles_per_pixel = footprint * 16.0;
    detail_weight = 1.0 - smoothstep(0.30, 0.68, cycles_per_pixel);
    return water_normal.Sample(water_normal_sampler, uv);
}

void EvaluateNormalMap(float2 world_xz, float time,
                       out float2 slope, out float height_detail) {
    float2 flow = normalize(flow_params.xy + float2(1e-5, 0.0));
    float2 side = float2(-flow.y, flow.x);
    float tiling = max(wave_params.w, 0.001);
    float speed = wave_params.z;

    float detail_a;
    float detail_b;
    float detail_c;
    float4 a = SampleNormalLayer(
        world_xz, 0.17, tiling,
        (flow * 0.027 + side * 0.007) * time * speed, detail_a);
    float4 b = SampleNormalLayer(
        world_xz, -0.91, tiling * 2.19,
        (-flow * 0.018 + side * 0.014) * time * speed
            + float2(0.31, 0.13), detail_b);
    float4 c = SampleNormalLayer(
        world_xz, 1.73, tiling * 4.47,
        (flow * 0.010 - side * 0.021) * time * speed
            + float2(0.17, 0.69), detail_c);

    float3 na = normalize(a.xyz * 2.0 - 1.0);
    float3 nb = normalize(b.xyz * 2.0 - 1.0);
    float3 nc = normalize(c.xyz * 2.0 - 1.0);
    float2 sa = Rotate2(na.xy / max(na.z, 0.20), -0.17);
    float2 sb = Rotate2(nb.xy / max(nb.z, 0.20), 0.91);
    float2 sc = Rotate2(nc.xy / max(nc.z, 0.20), -1.73);
    slope = sa * (0.54 * detail_a)
          + sb * (0.30 * detail_b)
          + sc * (0.16 * detail_c);
    height_detail = saturate(
        0.5
        + (a.a - 0.5) * (0.50 * detail_a)
        + (b.a - 0.5) * (0.32 * detail_b)
        + (c.a - 0.5) * (0.18 * detail_c));
}

float DistributionGGX(float no_h, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = no_h * no_h * (a2 - 1.0) + 1.0;
    return a2 / max(kPi * d * d, 1e-5);
}

float GeometrySchlickGGX(float no_x, float roughness) {
    float r = roughness + 1.0;
    float k = r * r * 0.125;
    return no_x / max(no_x * (1.0 - k) + k, 1e-4);
}

float3 FresnelSchlick(float cos_theta, float3 f0) {
    float one_minus_cos = 1.0 - saturate(cos_theta);
    float one_minus_cos2 = one_minus_cos * one_minus_cos;
    float fresnel_factor =
        one_minus_cos2 * one_minus_cos2 * one_minus_cos;
    return f0 + (1.0 - f0) * fresnel_factor;
}

float3 SampleSceneFiltered(float2 uv, float radius_pixels) {
    float2 texel = screen_params.xy;
    float2 safe_uv = clamp(uv, texel * 0.5, 1.0 - texel * 0.5);
    float2 ox = float2(texel.x * radius_pixels, 0.0);
    float2 oy = float2(0.0, texel.y * radius_pixels);
    float3 color = scene_color.Sample(scene_color_sampler, safe_uv).rgb * 0.50;
    color += scene_color.Sample(scene_color_sampler,
                                clamp(safe_uv + ox, texel, 1.0 - texel)).rgb * 0.125;
    color += scene_color.Sample(scene_color_sampler,
                                clamp(safe_uv - ox, texel, 1.0 - texel)).rgb * 0.125;
    color += scene_color.Sample(scene_color_sampler,
                                clamp(safe_uv + oy, texel, 1.0 - texel)).rgb * 0.125;
    color += scene_color.Sample(scene_color_sampler,
                                clamp(safe_uv - oy, texel, 1.0 - texel)).rgb * 0.125;
    return color;
}

float SceneUvFade(float2 uv) {
    float2 edge_pixels = min(uv, 1.0 - uv) / max(screen_params.xy, 1e-6);
    return saturate(min(edge_pixels.x, edge_pixels.y) * 0.20);
}

float3 ReconstructWorldPosition(float2 uv, float depth) {
    float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    clip.y = -clip.y;
    float4 world = mul(clip, inverse_view_projection);
    float safe_w = abs(world.w) > 1e-6
        ? world.w : (world.w < 0.0 ? -1e-6 : 1e-6);
    return world.xyz / safe_w;
}

float ComputeSunShadow(float3 world_position, float no_l) {
    float shadow_result = 1.0;
    if (shadow_params.x >= 0.5) {
        float4 light_clip =
            mul(float4(world_position, 1.0), light_view_projection);
        if (light_clip.w > 1e-5) {
            float3 light_ndc = light_clip.xyz / light_clip.w;
            bool inside_shadow_map =
                light_ndc.x >= -1.0 && light_ndc.x <= 1.0 &&
                light_ndc.y >= -1.0 && light_ndc.y <= 1.0 &&
                light_ndc.z >=  0.0 && light_ndc.z <= 1.0;
            if (inside_shadow_map) {
                float2 shadow_uv = float2(
                    light_ndc.x * 0.5 + 0.5,
                    -light_ndc.y * 0.5 + 0.5);
                float texel = max(abs(shadow_params.z), 1e-7);
                float radius = max(shadow_params.w, 0.0);
                float receiver_bias =
                    max(shadow_params.y, 0.0)
                    * lerp(1.0, 2.8, 1.0 - saturate(no_l));
                float2 min_uv = texel * 0.5;
                float2 max_uv = 1.0 - min_uv;

                // 5x5 tent-filtered PCF: stable enough for the high-energy sun
                // lobe while retaining contact definition on foam and crests.
                float visibility = 0.0;
                float weight_sum = 0.0;
                [unroll]
                for (int shadow_y = -2; shadow_y <= 2; ++shadow_y) {
                    [unroll]
                    for (int shadow_x = -2;
                         shadow_x <= 2;
                         ++shadow_x) {
                        float weight =
                            float(3 - abs(shadow_x))
                            * float(3 - abs(shadow_y));
                        float2 offset =
                            float2(float(shadow_x), float(shadow_y))
                            * (0.5 * radius * texel);
                        float stored_depth = shadow_map.SampleLevel(
                            shadow_map_sampler,
                            clamp(
                                shadow_uv + offset,
                                min_uv,
                                max_uv),
                            0).r;
                        visibility +=
                            (stored_depth + receiver_bias >= light_ndc.z
                                ? 1.0 : 0.0)
                            * weight;
                        weight_sum += weight;
                    }
                }
                shadow_result = visibility / max(weight_sum, 1e-4);
            }
        }
    }
    return shadow_result;
}

float4 PSMain(VSOut input) : SV_TARGET {
    float2 screen_uv = input.position.xy * screen_params.xy;
    if (surface_misc.z >= 0.5) {
        float opaque_depth =
            scene_depth.SampleLevel(scene_depth_sampler, screen_uv, 0).r;
        // This branch replaces the fixed-function depth test when scene depth
        // is simultaneously bound as an SRV (and therefore cannot be a DSV).
        if (opaque_depth + 2e-5 < input.position.z) discard;
    }

    float2 micro_slope;
    float normal_height;
    EvaluateNormalMap(input.world_position.xz, camera_time.w,
                      micro_slope, normal_height);

    float normal_strength = max(deep_normal.w, 0.0);
    // Combine macro and texture normals in slope space. Adding encoded normal
    // components directly made micro-wave strength depend on the macro slope
    // and flattened steep crests.
    float macro_normal_y = max(abs(input.world_normal.y), 0.08);
    float2 macro_slope = -input.world_normal.xz / macro_normal_y;
    float2 combined_slope = macro_slope + micro_slope * normal_strength;
    float3 normal = normalize(float3(
        -combined_slope.x, 1.0, -combined_slope.y));
    float3 view_direction = normalize(camera_time.xyz - input.world_position);
    if (dot(normal, view_direction) < 0.0) normal = -normal;

    float3 light_direction = normalize(sun_direction.xyz);
    float no_v = saturate(dot(normal, view_direction));
    float no_l = saturate(dot(normal, light_direction));
    float sun_visibility =
        ComputeSunShadow(input.world_position, no_l);
    const float3 water_f0 = float3(0.02037, 0.02037, 0.02037);
    float3 fresnel = FresnelSchlick(no_v, water_f0);

    // Screen-space Snell refraction from an opaque-scene copy.
    float3 refracted_direction = refract(-view_direction, normal, 1.0 / 1.333);
    if (dot(refracted_direction, refracted_direction) < 1e-4)
        refracted_direction = -view_direction;
    float2 distortion = normal.xz / max(abs(normal.y), 0.24);
    float refraction_strength = max(absorption_refraction.w, 0.0);
    float2 refract_uv_raw = screen_uv
        + (distortion * 8.0 + refracted_direction.xz * 5.0)
        * screen_params.xy * refraction_strength;
    float refract_fade = SceneUvFade(refract_uv_raw);
    float2 refract_uv = clamp(refract_uv_raw,
                              screen_params.xy * 0.5,
                              1.0 - screen_params.xy * 0.5);
    float blur_radius = lerp(0.45, 2.4, saturate(shallow_roughness.w));
    float3 captured_scene = SampleSceneFiltered(refract_uv, blur_radius);

    float optical_depth = max(surface_misc.x, 0.01);
    float path_length = optical_depth
                      / max(abs(refracted_direction.y), 0.20);
    float contact_foam = 0.0;
    if (surface_misc.z >= 0.5) {
        float bottom_depth =
            scene_depth.SampleLevel(scene_depth_sampler, refract_uv, 0).r;
        if (bottom_depth < 0.9999) {
            float3 scene_world =
                ReconstructWorldPosition(refract_uv, bottom_depth);
            float3 water_to_scene = scene_world - input.world_position;
            float behind_surface = dot(water_to_scene, -view_direction);
            if (behind_surface > 0.0) {
                // Reconstructed world thickness replaces the scalar fallback.
                path_length = clamp(length(water_to_scene), 0.01, 48.0);
                contact_foam =
                    1.0 - smoothstep(0.08, 1.50, behind_surface);
            }
        }
    }
    float3 absorption = max(absorption_refraction.rgb, 0.0);
    float3 scattering = max(scattering_phase.rgb, 0.0);
    float3 extinction = absorption + scattering;
    float3 transmittance = exp(-extinction * path_length);
    float transmission_luma =
        dot(transmittance, float3(0.2126, 0.7152, 0.0722));
    float3 volume_color = lerp(deep_normal.rgb, shallow_roughness.rgb,
                               saturate(transmission_luma * 0.82));
    float scene_weight = saturate(surface_misc.y) * refract_fade;
    float3 bottom = lerp(volume_color, captured_scene, scene_weight);
    // Homogeneous single scattering. The HG phase term retains directional
    // under-water sunlight without the view-independent glow produced by a
    // plain color lerp. Its 1/(4*pi) normalization keeps energy finite.
    float phase_g = clamp(scattering_phase.w, -0.95, 0.95);
    float phase_cos = clamp(
        dot(refracted_direction, light_direction), -1.0, 1.0);
    float phase_denominator = max(
        1.0 + phase_g * phase_g - 2.0 * phase_g * phase_cos,
        1e-3);
    float phase = (1.0 - phase_g * phase_g)
        / (4.0 * kPi * pow(phase_denominator, 1.5));
    float3 scattering_integral = scattering
        * (1.0 - transmittance) / max(extinction, 1e-4);
    float3 direct_inscatter = sun_color.rgb * sun_visibility
        * phase * scattering_integral;
    float3 refracted_color = bottom * transmittance
        + volume_color * (1.0 - transmittance)
        + direct_inscatter;

    // Procedural sky reflection remains valid when no reflection probe is bound.
    float3 reflected_direction = reflect(-view_direction, normal);
    float sky_height = saturate(reflected_direction.y * 0.5 + 0.5);
    float horizon_base =
        abs(saturate(1.0 - abs(reflected_direction.y)));
    float horizon = pow(abs(horizon_base), 0.68);
    float3 sky_zenith = float3(0.10, 0.30, 0.68) * 1.45;
    float3 sky_horizon = float3(0.58, 0.72, 0.88) * 1.18;
    float3 reflected_sky = lerp(
        sky_horizon, sky_zenith, pow(abs(sky_height), 0.72));
    reflected_sky += sky_horizon * horizon * 0.16;

    float2 reflection_uv_raw = screen_uv
        - distortion * screen_params.xy * 5.0;
    float reflection_edge = SceneUvFade(reflection_uv_raw);
    float2 reflection_uv = clamp(
        reflection_uv_raw, screen_params.xy * 0.5,
        1.0 - screen_params.xy * 0.5);
    float4 reflected_screen = screen_reflection.SampleLevel(
        screen_reflection_sampler, reflection_uv, 0);
    float reflection_hit =
        saturate(surface_misc.w) * saturate(reflected_screen.a)
        * reflection_edge;
    reflected_sky =
        lerp(reflected_sky, reflected_screen.rgb, reflection_hit);

    float sun_alignment = saturate(dot(reflected_direction, light_direction));
    float sun_disk_power = lerp(1400.0, 180.0,
                                saturate(shallow_roughness.w * 2.2));
    float3 sun_reflection = sun_color.rgb
                          * pow(abs(sun_alignment), sun_disk_power)
                          * sun_visibility;
    reflected_sky += sun_reflection;

    float3 color = lerp(refracted_color, reflected_sky, fresnel);

    // Energy-conserving GGX sun lobe.
    float roughness = clamp(shallow_roughness.w
                            + saturate(input.ripple_energy) * 0.035,
                            0.035, 0.72);
    float3 half_direction = normalize(view_direction + light_direction);
    float no_h = saturate(dot(normal, half_direction));
    float vo_h = saturate(dot(view_direction, half_direction));
    float distribution = DistributionGGX(no_h, roughness);
    float geometry = GeometrySchlickGGX(no_v, roughness)
                   * GeometrySchlickGGX(no_l, roughness);
    float3 spec_fresnel = FresnelSchlick(vo_h, water_f0);
    float3 specular = distribution * geometry * spec_fresnel
                    / max(4.0 * no_v * no_l, 1e-4);
    color += min(
        specular * sun_color.rgb * no_l * sun_visibility, 8.0);

    // Contact foam follows persistent ripple fronts; high macro crests add sparse caps.
    float ripple_foam = smoothstep(0.075, 0.28, input.ripple_energy);
    float crest = smoothstep(wave_params.x * 0.68,
                             max(wave_params.x * 1.22, 0.01),
                             input.wave_height);
    float detail_mask = smoothstep(0.52, 0.78, normal_height);
    float foam = saturate((ripple_foam * (0.16 + detail_mask * 0.34)
                         + crest * detail_mask * 0.16
                         + contact_foam * (0.34 + detail_mask * 0.26))
                         * foam_depth.w);
    color = lerp(
        color,
        foam_depth.rgb
            * (1.0 + sun_color.rgb * (0.025 * sun_visibility)),
        foam);

    return float4(max(color, 0.0), 1.0);
}
)";

FVec2 Normalize2(FVec2 value) noexcept {
    const f64 length = std::hypot(
        static_cast<f64>(value.x), static_cast<f64>(value.y));
    if (!std::isfinite(length) || length <= 1e-6)
        return FVec2{1.0f, 0.0f};
    return FVec2{
        static_cast<f32>(static_cast<f64>(value.x) / length),
        static_cast<f32>(static_cast<f64>(value.y) / length)};
}

FVec3 Normalize3(FVec3 value) noexcept {
    const f64 length = std::hypot(
        static_cast<f64>(value.x),
        static_cast<f64>(value.y),
        static_cast<f64>(value.z));
    if (!std::isfinite(length) || length <= 1e-6)
        return FVec3{0.0f, 1.0f, 0.0f};
    return FVec3{
        static_cast<f32>(static_cast<f64>(value.x) / length),
        static_cast<f32>(static_cast<f64>(value.y) / length),
        static_cast<f32>(static_cast<f64>(value.z) / length)};
}

bool IsFinite(FVec2 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool IsFinite(FVec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z);
}

bool IsFinite(const FMat4& value) noexcept {
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            if (!std::isfinite(value.m[row][column])) return false;
        }
    }
    return true;
}

f32 ClampFinite(f32 value, f32 fallback, f32 minimum,
                f32 maximum) noexcept {
    if (!std::isfinite(value)) value = fallback;
    if (value < minimum) value = minimum;
    if (value > maximum) value = maximum;
    return value;
}

FVec3 ClampFinite(FVec3 value, FVec3 fallback, f32 minimum,
                  f32 maximum) noexcept {
    return FVec3{
        ClampFinite(value.x, fallback.x, minimum, maximum),
        ClampFinite(value.y, fallback.y, minimum, maximum),
        ClampFinite(value.z, fallback.z, minimum, maximum),
    };
}

FWaterSurface3DParams SanitizeParams(
    const FWaterSurface3DParams& source) noexcept {
    const FWaterSurface3DParams defaults{};
    FWaterSurface3DParams result = source;
    result.shallow_color = ClampFinite(
        source.shallow_color, defaults.shallow_color, 0.0f, 64.0f);
    result.deep_color = ClampFinite(
        source.deep_color, defaults.deep_color, 0.0f, 64.0f);
    result.absorption = ClampFinite(
        source.absorption, defaults.absorption, 0.0f, 64.0f);
    result.scattering = ClampFinite(
        source.scattering, defaults.scattering, 0.0f, 64.0f);
    result.phase_anisotropy = ClampFinite(
        source.phase_anisotropy, defaults.phase_anisotropy, -0.95f, 0.95f);
    result.foam_color = ClampFinite(
        source.foam_color, defaults.foam_color, 0.0f, 64.0f);

    result.flow_direction = IsFinite(source.flow_direction)
        ? Normalize2(source.flow_direction)
        : Normalize2(defaults.flow_direction);
    result.roughness = ClampFinite(
        source.roughness, defaults.roughness, 0.02f, 1.0f);
    result.normal_strength = ClampFinite(
        source.normal_strength, defaults.normal_strength, 0.0f, 4.0f);
    result.normal_tiling = ClampFinite(
        source.normal_tiling, defaults.normal_tiling, 0.0001f, 1024.0f);
    result.refraction_strength = ClampFinite(
        source.refraction_strength, defaults.refraction_strength, 0.0f, 16.0f);
    result.optical_depth = ClampFinite(
        source.optical_depth, defaults.optical_depth, 0.001f, 10000.0f);
    result.wave_amplitude = ClampFinite(
        source.wave_amplitude, defaults.wave_amplitude, 0.0f, 1000.0f);
    result.wave_scale = ClampFinite(
        source.wave_scale, defaults.wave_scale, 0.0001f, 1024.0f);
    result.wave_speed = ClampFinite(
        source.wave_speed, defaults.wave_speed, -256.0f, 256.0f);
    result.ripple_speed = ClampFinite(
        source.ripple_speed, defaults.ripple_speed, 0.0f, 256.0f);
    result.ripple_wavelength = ClampFinite(
        source.ripple_wavelength, defaults.ripple_wavelength, 0.025f, 1024.0f);
    result.ripple_lifetime = ClampFinite(
        source.ripple_lifetime, defaults.ripple_lifetime, 0.1f, 3600.0f);
    result.ripple_damping = ClampFinite(
        source.ripple_damping, defaults.ripple_damping, 0.0f, 64.0f);
    result.foam_intensity = ClampFinite(
        source.foam_intensity, defaults.foam_intensity, 0.0f, 8.0f);
    return result;
}

TResult<TUniquePtr<IRhiTexture>> CreateWaterNormalMap(IRhiDevice& device) noexcept {
    constexpr u32 kSize = 256;
    constexpr usize kBytes =
        static_cast<usize>(kSize) * static_cast<usize>(kSize) * 4u;
    FAllocator& allocator = DefaultAllocator();
    u8* pixels = static_cast<u8*>(allocator.Alloc(kBytes));
    if (!pixels) {
        return Err<TUniquePtr<IRhiTexture>>(
            ACS_ERR(Render, 710, "WaterSurface3D normal-map allocation failed"));
    }

    struct FNormalWave {
        f32 x;
        f32 y;
        f32 amplitude;
        f32 phase;
    };
    static constexpr FNormalWave kWaves[] = {
        {  1.0f,  2.0f, 0.180f, 0.31f },
        {  2.0f, -1.0f, 0.155f, 1.47f },
        {  3.0f,  5.0f, 0.112f, 2.11f },
        { -5.0f,  4.0f, 0.096f, 0.83f },
        {  7.0f,  3.0f, 0.077f, 2.74f },
        {  9.0f, -7.0f, 0.061f, 1.18f },
        { -11.0f, 8.0f, 0.049f, 2.39f },
        { 13.0f, 11.0f, 0.039f, 0.57f },
        { 16.0f, -9.0f, 0.031f, 1.92f },
    };
    // Height is authored in normalized tile units, so the analytic derivative
    // includes 2*pi*frequency. This conversion keeps the resulting tangent
    // slope in a water-like range instead of allowing high-frequency layers to
    // produce near-horizontal normals.
    constexpr f32 kNormalSlopeScale = 0.028f;

    const auto to_byte = [](f32 value) noexcept -> u8 {
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        return static_cast<u8>(value * 255.0f + 0.5f);
    };

    for (u32 y = 0; y < kSize; ++y) {
        const f32 v =
            (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kSize);
        for (u32 x = 0; x < kSize; ++x) {
            const f32 u =
                (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kSize);
            f32 derivative_u = 0.0f;
            f32 derivative_v = 0.0f;
            f32 height = 0.0f;
            for (const FNormalWave& wave : kWaves) {
                const f32 phase =
                    kTwoPi * (wave.x * u + wave.y * v) + wave.phase;
                const f32 sine = std::sin(phase);
                const f32 cosine = std::cos(phase);
                derivative_u += wave.amplitude * kTwoPi * wave.x
                              * cosine * kNormalSlopeScale;
                derivative_v += wave.amplitude * kTwoPi * wave.y
                              * cosine * kNormalSlopeScale;
                height += wave.amplitude * sine;
            }

            const f32 inverse_length =
                1.0f / std::sqrt(derivative_u * derivative_u
                                 + derivative_v * derivative_v + 1.0f);
            const f32 normal_x = -derivative_u * inverse_length;
            const f32 normal_y = -derivative_v * inverse_length;
            const f32 normal_z = inverse_length;
            const usize index =
                (static_cast<usize>(y) * kSize + x) * 4u;
            pixels[index + 0] = to_byte(normal_x * 0.5f + 0.5f);
            pixels[index + 1] = to_byte(normal_y * 0.5f + 0.5f);
            pixels[index + 2] = to_byte(normal_z * 0.5f + 0.5f);
            pixels[index + 3] = to_byte(height * 0.56f + 0.5f);
        }
    }

    FTextureDesc texture_description{};
    texture_description.width = kSize;
    texture_description.height = kSize;
    texture_description.format = EFormat::R8G8B8A8_UNorm;
    texture_description.initial_data = pixels;
    texture_description.initial_data_size = kBytes;
    auto result = CreateRhiTexture(device, texture_description);
    allocator.Free(pixels);
    return result;
}

TResult<TUniquePtr<IRhiTexture>> CreateSceneFallback(IRhiDevice& device) noexcept {
    const u8 pixel[4] = { 31, 64, 79, 255 };
    FTextureDesc description{};
    description.width = 1;
    description.height = 1;
    description.format = EFormat::R8G8B8A8_UNorm;
    description.initial_data = pixel;
    description.initial_data_size = sizeof(pixel);
    return CreateRhiTexture(device, description);
}

} // namespace

FWaterSurface3D::FWaterSurface3D() noexcept = default;

FWaterSurface3D::~FWaterSurface3D() noexcept {
    Shutdown();
}

void FWaterSurface3D::SetParams(
    const FWaterSurface3DParams& params) noexcept {
    m_Params = SanitizeParams(params);
}

TResult<void> FWaterSurface3D::Init(IRhiDevice& device, EFormat rt_format,
                                    EFormat depth_format,
                                    u32 msaa_samples) noexcept {
    Shutdown();
    m_Device = &device;

    FShaderDesc vertex_description{};
    vertex_description.stage = EShaderStage::Vertex;
    vertex_description.hlsl_source = kWaterSurface3DHlsl;
    vertex_description.entry_point = "VSMain";
    vertex_description.debug_name = "WaterSurface3D.VS";
    auto vertex_result = CreateRhiShader(device, vertex_description);
    if (vertex_result.IsErr()) {
        Shutdown();
        return Err<void>(vertex_result.Error());
    }
    m_Vs = Move(vertex_result.Value());

    FShaderDesc pixel_description{};
    pixel_description.stage = EShaderStage::Pixel;
    pixel_description.hlsl_source = kWaterSurface3DHlsl;
    pixel_description.entry_point = "PSMain";
    pixel_description.debug_name = "WaterSurface3D.PS";
    auto pixel_result = CreateRhiShader(device, pixel_description);
    if (pixel_result.IsErr()) {
        Shutdown();
        return Err<void>(pixel_result.Error());
    }
    m_Ps = Move(pixel_result.Value());

    for (u32 i = 0; i < kConstantBufferRing; ++i) {
        FBufferDesc frame_description{};
        frame_description.size = ConstantBufferSize<FWaterFrameCb>();
        frame_description.usage = EBufferUsage::Uniform;
        frame_description.cpu_writable = true;
        auto frame_result = CreateRhiBuffer(device, frame_description);
        if (frame_result.IsErr()) {
            Shutdown();
            return Err<void>(frame_result.Error());
        }
        m_FrameCb[i] = Move(frame_result.Value());

        FBufferDesc object_description{};
        object_description.size = ConstantBufferSize<FWaterObjectCb>();
        object_description.usage = EBufferUsage::Uniform;
        object_description.cpu_writable = true;
        auto object_result = CreateRhiBuffer(device, object_description);
        if (object_result.IsErr()) {
            Shutdown();
            return Err<void>(object_result.Error());
        }
        m_ObjectCb[i] = Move(object_result.Value());
    }

    auto normal_result = CreateWaterNormalMap(device);
    if (normal_result.IsErr()) {
        Shutdown();
        return Err<void>(normal_result.Error());
    }
    m_NormalMap = Move(normal_result.Value());

    auto fallback_result = CreateSceneFallback(device);
    if (fallback_result.IsErr()) {
        Shutdown();
        return Err<void>(fallback_result.Error());
    }
    m_SceneFallback = Move(fallback_result.Value());

    FPipelineDesc pipeline_description{};
    pipeline_description.vs = m_Vs.Get();
    pipeline_description.ps = m_Ps.Get();
    pipeline_description.topology = EPrimitiveTopology::TriangleList;
    pipeline_description.rt_format = rt_format;
    pipeline_description.depth_format = depth_format;
    pipeline_description.depth_test = depth_format != EFormat::Unknown;
    pipeline_description.depth_write = true;
    pipeline_description.cull_mode = ECullMode::None;
    pipeline_description.blend_mode = EBlendMode::Opaque;
    pipeline_description.sample_count = msaa_samples > 0 ? msaa_samples : 1;
    pipeline_description.vertex_stride = sizeof(FMeshVertex);
    pipeline_description.layout[0] =
        { "POSITION", 0, EFormat::R32G32B32_Float, 0 };
    pipeline_description.layout[1] =
        { "NORMAL", 0, EFormat::R32G32B32_Float, 16 };
    pipeline_description.layout[2] =
        { "TEXCOORD", 0, EFormat::R32G32_Float, 32 };
    pipeline_description.layout_count = 3;
    pipeline_description.cbuffer_slots = 2;
    pipeline_description.cbuffer_names[0] = "WaterFrame";
    pipeline_description.cbuffer_names[1] = "WaterObject";
    pipeline_description.texture_slots = 5;
    pipeline_description.texture_names[0] = "water_normal";
    pipeline_description.texture_names[1] = "scene_color";
    pipeline_description.texture_names[2] = "scene_depth";
    pipeline_description.texture_names[3] = "screen_reflection";
    pipeline_description.texture_names[4] = "shadow_map";
    pipeline_description.static_sampler_count = 5;
    pipeline_description.static_samplers[0].filter = ESamplerFilter::Linear;
    pipeline_description.static_samplers[0].address_u = ESamplerAddress::Wrap;
    pipeline_description.static_samplers[0].address_v = ESamplerAddress::Wrap;
    pipeline_description.static_samplers[1].filter = ESamplerFilter::Linear;
    pipeline_description.static_samplers[1].address_u = ESamplerAddress::Clamp;
    pipeline_description.static_samplers[1].address_v = ESamplerAddress::Clamp;
    pipeline_description.static_samplers[2].filter = ESamplerFilter::Point;
    pipeline_description.static_samplers[2].address_u = ESamplerAddress::Clamp;
    pipeline_description.static_samplers[2].address_v = ESamplerAddress::Clamp;
    pipeline_description.static_samplers[3].filter = ESamplerFilter::Linear;
    pipeline_description.static_samplers[3].address_u = ESamplerAddress::Clamp;
    pipeline_description.static_samplers[3].address_v = ESamplerAddress::Clamp;
    pipeline_description.static_samplers[4].filter = ESamplerFilter::Point;
    pipeline_description.static_samplers[4].address_u = ESamplerAddress::Clamp;
    pipeline_description.static_samplers[4].address_v = ESamplerAddress::Clamp;

    auto pipeline_result = CreateRhiPipeline(device, pipeline_description);
    if (pipeline_result.IsErr()) {
        Shutdown();
        return Err<void>(pipeline_result.Error());
    }
    m_Pipeline = Move(pipeline_result.Value());

    // A depth texture cannot be a DSV and SRV at the same time. This variant
    // targets a color-only pass and lets PSMain perform the equivalent opaque
    // scene test while sampling the shader-visible depth.
    pipeline_description.depth_format = EFormat::Unknown;
    pipeline_description.depth_test = false;
    pipeline_description.depth_write = false;
    auto manual_depth_result =
        CreateRhiPipeline(device, pipeline_description);
    if (manual_depth_result.IsErr()) {
        Shutdown();
        return Err<void>(manual_depth_result.Error());
    }
    m_ManualDepthPipeline = Move(manual_depth_result.Value());
    m_FrameSlot = 0;
    m_DrawCursor = 0;
    m_DrawOverflowLogged = false;
    m_Time = 0.0f;
    ClearDisturbances();
    return Ok();
}

void FWaterSurface3D::Shutdown() noexcept {
    m_ManualDepthPipeline.Reset();
    m_Pipeline.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
    for (u32 i = 0; i < kConstantBufferRing; ++i) {
        m_ObjectCb[i].Reset();
        m_FrameCb[i].Reset();
    }
    m_SceneFallback.Reset();
    m_NormalMap.Reset();
    m_ShadowMap = nullptr;
    m_Device = nullptr;
    m_FrameSlot = 0;
    m_DrawCursor = 0;
    m_DrawOverflowLogged = false;
}

void FWaterSurface3D::Update(f32 dt) noexcept {
    if (dt < 0.0f) dt = 0.0f;
    if (!std::isfinite(dt)) return;
    const f64 next_time = static_cast<f64>(m_Time) + static_cast<f64>(dt);
    // The shader only needs a periodic phase clock. Wrapping every update also
    // keeps phase multiplication precise when a caller supplies a huge but
    // finite hitch delta; never expose FLT_MAX to sin/cos for even one frame.
    m_Time = static_cast<f32>(std::fmod(next_time, 65536.0));

    for (u32 i = 0; i < kMaxRipples; ++i) {
        FRipple& ripple = m_Ripples[i];
        if (!ripple.active) continue;
        const f64 next_age =
            static_cast<f64>(ripple.age) + static_cast<f64>(dt);
        if (next_age >= static_cast<f64>(ripple.lifetime)) {
            ripple.active = false;
            continue;
        }
        ripple.age = static_cast<f32>(next_age);
        ripple.amplitude =
            ripple.initial_amplitude
            * std::exp(-m_Params.ripple_damping * ripple.age);
        if (ripple.age >= ripple.lifetime
            || std::abs(ripple.amplitude) < 0.0015f) {
            ripple.active = false;
        }
    }
}

bool FWaterSurface3D::AddEvent(FVec3 world_point, FVec2 direction,
                               f32 anisotropy, f32 radius,
                               f32 strength,
                               u32 first_slot, u32 slot_count) noexcept {
    if (!IsFinite(world_point) || !IsFinite(direction)
        || !std::isfinite(anisotropy) || !std::isfinite(radius)
        || !std::isfinite(strength) || std::abs(strength) < 1e-6f) {
        return false;
    }
    if (radius < 0.0f) radius = 0.0f;
    if (radius > 1.0e15f) radius = 1.0e15f;
    if (strength > 65504.0f) strength = 65504.0f;
    if (strength < -65504.0f) strength = -65504.0f;
    world_point = ClampFinite(
        world_point, FVec3{0.0f, 0.0f, 0.0f}, -1.0e15f, 1.0e15f);
    if (anisotropy < 1.0f) anisotropy = 1.0f;
    if (anisotropy > 3.5f) anisotropy = 3.5f;
    if (first_slot >= kMaxRipples || slot_count == 0) return false;
    u32 end_slot = first_slot + slot_count;
    if (end_slot < first_slot || end_slot > kMaxRipples) {
        end_slot = kMaxRipples;
    }

    for (u32 i = first_slot; i < end_slot; ++i) {
        FRipple& ripple = m_Ripples[i];
        if (ripple.active) continue;
        ripple.center = FVec2{world_point.x, world_point.z};
        ripple.direction = Normalize2(direction);
        ripple.initial_radius = radius;
        ripple.initial_amplitude = strength;
        ripple.amplitude = strength;
        ripple.age = 0.0f;
        ripple.speed =
            m_Params.ripple_speed > 0.0f ? m_Params.ripple_speed : 0.0f;
        ripple.lifetime =
            m_Params.ripple_lifetime > 0.1f ? m_Params.ripple_lifetime : 0.1f;
        ripple.anisotropy = anisotropy;
        ripple.active = true;
        return true;
    }

    // Persistence is deliberate: never replace an active visible event.
    return false;
}

bool FWaterSurface3D::AddDisturbance(FVec3 world_point, f32 radius,
                                     f32 strength) noexcept {
    return AddEvent(world_point, FVec2{1.0f, 0.0f}, 1.0f,
                    radius, strength, 0, kImpactRippleSlots);
}

bool FWaterSurface3D::AddWake(FVec3 world_point, FVec3 world_velocity,
                              f32 radius, f32 strength) noexcept {
    if (!IsFinite(world_velocity)) return false;
    const f32 speed = std::hypot(world_velocity.x, world_velocity.z);
    const FVec2 direction = speed < 1e-4f
        ? FVec2{1.0f, 0.0f}
        : Normalize2(FVec2{world_velocity.x, world_velocity.z});
    const f32 anisotropy =
        1.35f + (speed < 8.0f ? speed * 0.16f : 1.28f);
    const FVec3 trailing_point{
        world_point.x - direction.x * radius * 0.38f,
        world_point.y,
        world_point.z - direction.y * radius * 0.38f,
    };
    if (!IsFinite(trailing_point)) return false;
    return AddEvent(trailing_point, direction, anisotropy,
                    radius, strength,
                    kImpactRippleSlots, kWakeRippleSlots);
}

void FWaterSurface3D::ClearDisturbances() noexcept {
    for (u32 i = 0; i < kMaxRipples; ++i) {
        m_Ripples[i] = FRipple{};
    }
}

u32 FWaterSurface3D::ActiveRippleCount() const noexcept {
    u32 count = 0;
    for (u32 i = 0; i < kMaxRipples; ++i) {
        if (m_Ripples[i].active) ++count;
    }
    return count;
}

void FWaterSurface3D::SetFrame(const FMat4& view_projection,
                               FVec3 camera_pos,
                               u32 screen_width, u32 screen_height,
                               FVec3 sun_direction,
                               FVec3 sun_color) noexcept {
    if (IsFinite(view_projection)) {
        const FMat4 inverse = Inverse(view_projection);
        if (IsFinite(inverse)) {
            m_ViewProjection = view_projection;
            m_InverseViewProjection = inverse;
        }
    }
    m_CameraPos = ClampFinite(
        camera_pos, m_CameraPos, -1.0e15f, 1.0e15f);
    m_ScreenWidth = screen_width > 0 ? screen_width : 1;
    m_ScreenHeight = screen_height > 0 ? screen_height : 1;
    if (IsFinite(sun_direction)) m_SunDirection = Normalize3(sun_direction);
    m_SunColor = ClampFinite(sun_color, m_SunColor, 0.0f, 65504.0f);
    m_FrameSlot = (m_FrameSlot + 1u) % kBufferedFrames;
    m_DrawCursor = 0;
    m_DrawOverflowLogged = false;
}

void FWaterSurface3D::SetShadowMap(
    IRhiTexture* shadow_map,
    const FMat4& light_view_projection,
    f32 depth_bias,
    f32 pcf_radius) noexcept {
    const bool valid_projection = IsFinite(light_view_projection);
    m_ShadowMap = valid_projection ? shadow_map : nullptr;
    m_LightViewProjection = valid_projection
        ? light_view_projection : FMat4::Identity();
    m_ShadowBias =
        std::isfinite(depth_bias) && depth_bias > 0.0f ? depth_bias : 0.0f;
    m_ShadowPcfRadius = ClampFinite(pcf_radius, 0.0f, 0.0f, 8.0f);
}

void FWaterSurface3D::DrawMesh(IRhiCommandList& command_list,
                               const FGpuMesh& mesh,
                               const FMat4& model,
                               IRhiTexture* scene_color,
                               IRhiTexture* scene_depth,
                               IRhiTexture* screen_reflection) noexcept {
    if (!m_Pipeline || !m_ManualDepthPipeline || !IsFinite(model)
        || !mesh.vertex_buffer || !mesh.index_buffer
        || !m_NormalMap || !m_SceneFallback) {
        return;
    }

    if (m_DrawCursor >= kMaxDrawsPerFrame) {
        if (!m_DrawOverflowLogged) {
            ACS_LOG_WARN(
                "FWaterSurface3D: more than %u DrawMesh calls after one "
                "SetFrame; extra draws skipped to preserve constant buffers",
                kMaxDrawsPerFrame);
            m_DrawOverflowLogged = true;
        }
        return;
    }
    const u32 buffer_index =
        m_FrameSlot * kMaxDrawsPerFrame + m_DrawCursor;
    ++m_DrawCursor;
    IRhiBuffer* frame_buffer = m_FrameCb[buffer_index].Get();
    IRhiBuffer* object_buffer = m_ObjectCb[buffer_index].Get();
    if (!frame_buffer || !object_buffer) return;

    FWaterFrameCb frame{};
    frame.view_projection = m_ViewProjection;
    frame.inverse_view_projection = m_InverseViewProjection;
    frame.light_view_projection = m_LightViewProjection;
    frame.camera_time =
        FVec4{m_CameraPos.x, m_CameraPos.y, m_CameraPos.z, m_Time};
    frame.screen_params = FVec4{
        1.0f / static_cast<f32>(m_ScreenWidth),
        1.0f / static_cast<f32>(m_ScreenHeight),
        static_cast<f32>(m_ScreenWidth),
        static_cast<f32>(m_ScreenHeight),
    };
    frame.sun_direction =
        FVec4{m_SunDirection.x, m_SunDirection.y, m_SunDirection.z, 1.0f};
    frame.sun_color =
        FVec4{m_SunColor.x, m_SunColor.y, m_SunColor.z, 1.0f};
    frame.shallow_roughness =
        FVec4{m_Params.shallow_color.x, m_Params.shallow_color.y,
              m_Params.shallow_color.z, m_Params.roughness};
    frame.deep_normal =
        FVec4{m_Params.deep_color.x, m_Params.deep_color.y,
              m_Params.deep_color.z, m_Params.normal_strength};
    frame.absorption_refraction =
        FVec4{m_Params.absorption.x, m_Params.absorption.y,
              m_Params.absorption.z, m_Params.refraction_strength};
    frame.scattering_phase =
        FVec4{m_Params.scattering.x, m_Params.scattering.y,
              m_Params.scattering.z, m_Params.phase_anisotropy};
    frame.foam_depth =
        FVec4{m_Params.foam_color.x, m_Params.foam_color.y,
              m_Params.foam_color.z, m_Params.foam_intensity};
    frame.wave_params =
        FVec4{m_Params.wave_amplitude, m_Params.wave_scale,
              m_Params.wave_speed, m_Params.normal_tiling};
    const FVec2 normalized_flow = Normalize2(m_Params.flow_direction);

    u32 ripple_count = 0;
    for (u32 i = 0; i < kMaxRipples; ++i) {
        const FRipple& ripple = m_Ripples[i];
        if (!ripple.active) continue;
        const f32 remaining =
            1.0f - ripple.age / (ripple.lifetime > 0.0f
                                    ? ripple.lifetime : 1.0f);
        frame.ripple_a[ripple_count] =
            FVec4{ripple.center.x, ripple.center.y,
                  ripple.initial_radius + ripple.speed * ripple.age,
                  ripple.amplitude};
        frame.ripple_b[ripple_count] =
            FVec4{ripple.direction.x, ripple.direction.y,
                  ripple.anisotropy,
                  remaining > 0.0f ? remaining : 0.0f};
        ++ripple_count;
    }
    frame.flow_params =
        FVec4{normalized_flow.x, normalized_flow.y,
              m_Params.ripple_wavelength,
              static_cast<f32>(ripple_count)};
    frame.surface_misc =
        FVec4{m_Params.optical_depth,
              scene_color ? 1.0f : 0.0f,
              scene_depth ? 1.0f : 0.0f,
              screen_reflection ? 1.0f : 0.0f};
    const f32 shadow_texel_size =
        m_ShadowMap && m_ShadowMap->Width() > 0
        ? 1.0f / static_cast<f32>(m_ShadowMap->Width())
        : 0.0f;
    frame.shadow_params =
        FVec4{m_ShadowMap ? 1.0f : 0.0f,
              m_ShadowBias, shadow_texel_size, m_ShadowPcfRadius};
    frame_buffer->Update(&frame, sizeof(frame));

    FWaterObjectCb object{};
    object.model = model;
    object_buffer->Update(&object, sizeof(object));

    IRhiPipeline* pipeline = scene_depth
        ? m_ManualDepthPipeline.Get()
        : m_Pipeline.Get();
    command_list.SetPipeline(*pipeline);
    command_list.SetConstantBuffer(0, *frame_buffer);
    command_list.SetConstantBuffer(1, *object_buffer);
    command_list.SetTexture(0, *m_NormalMap);
    command_list.SetTexture(
        1, scene_color ? *scene_color : *m_SceneFallback);
    command_list.SetTexture(
        2, scene_depth ? *scene_depth : *m_SceneFallback);
    command_list.SetTexture(
        3, screen_reflection
            ? *screen_reflection : *m_SceneFallback);
    command_list.SetTexture(
        4, m_ShadowMap ? *m_ShadowMap : *m_SceneFallback);
    command_list.SetVertexBuffer(*mesh.vertex_buffer, mesh.vertex_stride);
    command_list.SetIndexBuffer(*mesh.index_buffer);
    command_list.DrawIndexed(mesh.index_count);
}

} // namespace acs
