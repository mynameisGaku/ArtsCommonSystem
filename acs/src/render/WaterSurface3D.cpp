// SPDX-License-Identifier: Apache-2.0
#include "render/WaterSurface3D.h"

#if !WITH_RENDER_DILIGENT
#include "render/Dx12/Dx12Shader.h"
#endif
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
#include "render/NormalMatrix.h"

#include <algorithm>
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
    FVec4 author_normal_params;
    FVec4 shadow_params;
    FVec4 environment_zenith;
    FVec4 environment_horizon;
    FVec4 environment_ground;
    FVec4 ripple_a[CWaterSurface3D::kMaxRipples];
    FVec4 ripple_b[CWaterSurface3D::kMaxRipples];
};

struct FWaterObjectCb {
    FMat4 model;
    FVec4 normal_row0;
    FVec4 normal_row1;
    FVec4 normal_row2;
    FVec4 surface_origin;
    FVec4 surface_tangent;
    FVec4 surface_bitangent;
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
    float4 author_normal_params; // x=enabled, y=slope strength
    float4 shadow_params;        // x=enabled, y=bias, z=texel size, w=PCF radius
    float4 environment_zenith;   // rgb=actual sky radiance toward world +Y
    float4 environment_horizon;  // rgb=actual sky radiance at the horizon
    float4 environment_ground;   // rgb=actual environment radiance toward world -Y
    float4 ripple_a[kMaxRipples];// xy=center XZ, z=current radius, w=current amplitude
    float4 ripple_b[kMaxRipples];// xy=direction XZ, z=anisotropy, w=remaining life
};

cbuffer WaterObject : register(b1) {
    float4x4 model;
    float4 normal_row0;
    float4 normal_row1;
    float4 normal_row2;
    float4 surface_origin;
    float4 surface_tangent;
    float4 surface_bitangent;
};

Texture2D water_normal : register(t0);
SamplerState water_normal_sampler : register(s0);
Texture2D authored_normal : register(t1);
SamplerState authored_normal_sampler : register(s1);
Texture2D scene_color : register(t2);
SamplerState scene_color_sampler : register(s2);
Texture2D scene_depth : register(t3);
SamplerState scene_depth_sampler : register(s3);
Texture2D screen_reflection : register(t4);
SamplerState screen_reflection_sampler : register(s4);
Texture2D shadow_map : register(t5);
SamplerState shadow_map_sampler : register(s5);

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
    float2 surface_position : TEXCOORD3;
    float3 surface_tangent : TEXCOORD4;
    float3 surface_bitangent : TEXCOORD5;
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
    float3 base_normal = float3(
        input.normal.x * normal_row0.x
            + input.normal.y * normal_row1.x
            + input.normal.z * normal_row2.x,
        input.normal.x * normal_row0.y
            + input.normal.y * normal_row1.y
            + input.normal.z * normal_row2.y,
        input.normal.x * normal_row0.z
            + input.normal.y * normal_row1.z
            + input.normal.z * normal_row2.z);
    float base_normal_length = length(base_normal);
    base_normal = base_normal_length > 1e-5
        ? base_normal / base_normal_length
        : normalize(cross(surface_bitangent.xyz,
                          surface_tangent.xyz));
    float3 frame_tangent = normalize(surface_tangent.xyz);
    float3 frame_bitangent = normalize(surface_bitangent.xyz);
    float3 tangent_candidate =
        frame_tangent - base_normal * dot(frame_tangent, base_normal);
    float tangent_length = length(tangent_candidate);
    float3 tangent = tangent_length > 1e-5
        ? tangent_candidate / tangent_length
        : normalize(cross(base_normal, frame_bitangent));
    float3 bitangent = normalize(cross(tangent, base_normal));
    if (dot(bitangent, frame_bitangent) < 0.0) bitangent = -bitangent;
    float3 from_origin = world.xyz - surface_origin.xyz;
    float2 surface_position = float2(
        dot(from_origin, frame_tangent),
        dot(from_origin, frame_bitangent));

    float ambient_height;
    float2 ambient_gradient;
    EvaluateAmbientWaves(surface_position, camera_time.w,
                         ambient_height, ambient_gradient);

    float ripple_height;
    float2 ripple_gradient;
    float ripple_energy;
    EvaluateRipples(surface_position, ripple_height,
                    ripple_gradient, ripple_energy);

    float2 total_gradient = ambient_gradient + ripple_gradient;
    world.xyz += base_normal * (ambient_height + ripple_height);
    output.world_position = world.xyz;
    // Keep this vector unnormalized through interpolation. The pixel shader
    // adds texture-slope detail in the same tangent frame, then performs the
    // only normalization so mesh, analytic, and normal-map normals stay
    // coherent under rotated/non-uniformly-scaled models.
    output.world_normal = base_normal
                        - tangent * total_gradient.x
                        - bitangent * total_gradient.y;
    output.position = mul(world, view_projection);
    output.uv = input.uv;
    output.ripple_energy = ripple_energy;
    output.wave_height = ambient_height + ripple_height;
    output.surface_position = surface_position;
    output.surface_tangent = tangent;
    output.surface_bitangent = bitangent;
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

float3 EvaluateAuthoredNormal(float2 mesh_uv) {
    float3 authored_result = float3(0.0, 0.0, 1.0);
    if (author_normal_params.x >= 0.5) {
        float3 sampled = authored_normal.Sample(
            authored_normal_sampler, mesh_uv).xyz * 2.0 - 1.0;
        float sampled_length = length(sampled);
        if (sampled_length > 1e-5) {
            sampled /= sampled_length;
            // Strength scales slope, not an already normalized direction.
            // This preserves a positive tangent-space hemisphere at high
            // strengths.
            float2 slope = sampled.xy / max(abs(sampled.z), 0.20);
            slope *= max(author_normal_params.y, 0.0);
            authored_result = normalize(float3(slope, 1.0));
        }
    }
    return authored_result;
}

float3 PerturbWaterNormal(
    float3 world_position, float3 world_normal, float2 mesh_uv,
    float3 tangent_normal,
    float3 fallback_tangent, float3 fallback_bitangent) {
    // Schueler derivative TBN: authored UV rotation and scale remain correct
    // without requiring vertex tangents in the compact water mesh layout.
    float3 dp_dx = ddx(world_position);
    float3 dp_dy = ddy(world_position);
    float2 duv_dx = ddx(mesh_uv);
    float2 duv_dy = ddy(mesh_uv);
    float3 dp_dy_perp = cross(dp_dy, world_normal);
    float3 dp_dx_perp = cross(world_normal, dp_dx);
    float3 uv_tangent =
        dp_dy_perp * duv_dx.x + dp_dx_perp * duv_dy.x;
    float3 uv_bitangent =
        dp_dy_perp * duv_dx.y + dp_dx_perp * duv_dy.y;
    float frame_extent = max(
        dot(uv_tangent, uv_tangent),
        dot(uv_bitangent, uv_bitangent));
    float3 perturbed_result = world_normal;
    if (frame_extent <= 1e-10) {
        // Degenerate/custom UVs still receive the authored normal in the
        // validated local-XZ surface frame, never a NaN or black pixel.
        float3 safe_tangent =
            fallback_tangent
            - world_normal
              * dot(fallback_tangent, world_normal);
        float safe_tangent_length = length(safe_tangent);
        safe_tangent = safe_tangent_length > 1e-5
            ? safe_tangent / safe_tangent_length
            : normalize(cross(world_normal, fallback_bitangent));
        float3 safe_bitangent =
            normalize(cross(safe_tangent, world_normal));
        if (dot(safe_bitangent, fallback_bitangent) < 0.0)
            safe_bitangent = -safe_bitangent;
        perturbed_result = normalize(
            safe_tangent * tangent_normal.x
            + safe_bitangent * tangent_normal.y
            + world_normal * tangent_normal.z);
    } else {
        float inverse_extent = rsqrt(frame_extent);
        perturbed_result = normalize(
            uv_tangent * (tangent_normal.x * inverse_extent)
            + uv_bitangent * (tangent_normal.y * inverse_extent)
            + world_normal * tangent_normal.z);
    }
    return perturbed_result;
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

float2 ProjectWorldDirectionToScreenPixels(float3 world_position,
                                           float3 world_direction) {
    float2 projected_result = float2(0.0, 0.0);
    float direction_length = length(world_direction);
    if (direction_length > 1e-5) {
        float3 direction = world_direction / direction_length;
        float4 origin_clip =
            mul(float4(world_position, 1.0), view_projection);
        float4 tip_clip =
            mul(float4(world_position + direction, 1.0), view_projection);
        if (abs(origin_clip.w) > 1e-5 && abs(tip_clip.w) > 1e-5) {
            float2 origin_ndc = origin_clip.xy / origin_clip.w;
            float2 tip_ndc = tip_clip.xy / tip_clip.w;
            float2 screen_delta =
                (tip_ndc - origin_ndc) * float2(0.5, -0.5);
            float2 pixel_delta = screen_delta * screen_params.zw;
            float pixel_length = length(pixel_delta);
            if (pixel_length > 1e-4)
                projected_result = pixel_delta / pixel_length;
        }
    }
    return projected_result;
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
    EvaluateNormalMap(input.surface_position, camera_time.w,
                      micro_slope, normal_height);
    float3 authored_tangent_normal =
        EvaluateAuthoredNormal(input.uv);

    float normal_strength = max(deep_normal.w, 0.0);
    // The VS carries the mesh normal plus analytic macro slopes. Generated
    // world detail is added in the transformed surface frame; the authored
    // mesh-UV normal is then composed through a derivative TBN. This uses the
    // authored vertex normals instead of replacing every mesh normal with +Y.
    float3 tangent = normalize(input.surface_tangent);
    float3 bitangent = normalize(input.surface_bitangent);
    float3 view_direction =
        normalize(camera_time.xyz - input.world_position);
    float3 geometric_surface_normal = normalize(input.world_normal);
    if (dot(geometric_surface_normal, view_direction) < 0.0)
        geometric_surface_normal = -geometric_surface_normal;
    float3 generated_detail_normal = normalize(
        geometric_surface_normal
        + tangent * (micro_slope.x * normal_strength)
        + bitangent * (micro_slope.y * normal_strength));
    float3 normal = author_normal_params.x >= 0.5
        ? PerturbWaterNormal(
              input.world_position, generated_detail_normal,
              input.uv, authored_tangent_normal,
              tangent, bitangent)
        : generated_detail_normal;
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
    float2 projected_normal =
        ProjectWorldDirectionToScreenPixels(input.world_position, normal);
    float2 projected_refracted_ray =
        ProjectWorldDirectionToScreenPixels(
            input.world_position, refracted_direction);
    float refraction_strength = max(absorption_refraction.w, 0.0);
    float2 refract_uv_raw = screen_uv
        + (projected_normal * 8.0 + projected_refracted_ray * 5.0)
        * screen_params.xy * refraction_strength;
    float refract_fade = SceneUvFade(refract_uv_raw);
    float2 refract_uv = clamp(refract_uv_raw,
                              screen_params.xy * 0.5,
                              1.0 - screen_params.xy * 0.5);
    float blur_radius = lerp(0.45, 2.4, saturate(shallow_roughness.w));
    float3 captured_scene = SampleSceneFiltered(refract_uv, blur_radius);

    float optical_depth = max(surface_misc.x, 0.01);
    float path_length = optical_depth
                      / max(abs(dot(refracted_direction,
                                    geometric_surface_normal)), 0.05);
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

    // The caller supplies the same environment colors used by the visible sky.
    // This keeps the no-probe fallback coherent across day/night presets.
    float3 reflected_direction = reflect(-view_direction, normal);
    float environment_height = reflected_direction.y;
    float environment_weight = pow(
        saturate(abs(environment_height)), 0.72);
    float3 reflected_sky = environment_height >= 0.0
        ? lerp(environment_horizon.rgb,
               environment_zenith.rgb, environment_weight)
        : lerp(environment_horizon.rgb,
               environment_ground.rgb, environment_weight);

    float2 reflection_uv_raw = screen_uv
        - projected_normal * screen_params.xy * 5.0;
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
    // A saturating optical response keeps foam visible, however faintly,
    // until the same C2 disturbance envelope reaches zero. The former
    // smoothstep threshold made foam pop off while displacement and its
    // analytic normal still had a finite contribution.
    float ripple_foam =
        1.0 - exp(-max(input.ripple_energy, 0.0) * 5.4);
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

struct FWaterSurfaceFrame {
    FVec3 origin{0.0f, 0.0f, 0.0f};
    FVec3 tangent{1.0f, 0.0f, 0.0f};
    FVec3 bitangent{0.0f, 0.0f, 1.0f};
    FVec3 normal{0.0f, 1.0f, 0.0f};
    FMat4 normal_matrix = FMat4::Identity();
};

FVec3 Normalize3Or(FVec3 value, FVec3 fallback) noexcept {
    if (!IsFinite(value)) return fallback;
    const f64 length = std::hypot(
        static_cast<f64>(value.x),
        static_cast<f64>(value.y),
        static_cast<f64>(value.z));
    if (!std::isfinite(length) || length <= 1e-8) return fallback;
    return FVec3{
        static_cast<f32>(static_cast<f64>(value.x) / length),
        static_cast<f32>(static_cast<f64>(value.y) / length),
        static_cast<f32>(static_cast<f64>(value.z) / length),
    };
}

FWaterSurfaceFrame BuildWaterSurfaceFrame(const FMat4& model) noexcept {
    FWaterSurfaceFrame result{};
    result.normal_matrix = MakeSafeNormalMatrix(model);

    const FVec3 transformed_origin =
        TransformPoint(FVec3::Zero(), model);
    if (IsFinite(transformed_origin)) result.origin = transformed_origin;

    result.normal = Normalize3Or(
        TransformVector(FVec3::UnitY(), result.normal_matrix),
        FVec3::UnitY());

    FVec3 transformed_tangent =
        TransformVector(FVec3::UnitX(), model);
    transformed_tangent =
        transformed_tangent
        - result.normal * Dot(transformed_tangent, result.normal);
    if (!IsFinite(transformed_tangent)
        || std::hypot(
               static_cast<f64>(transformed_tangent.x),
               static_cast<f64>(transformed_tangent.y),
               static_cast<f64>(transformed_tangent.z)) <= 1e-8) {
        // Choose the world axis least likely to be parallel to the normal.
        const FVec3 reference = std::abs(result.normal.z) < 0.9f
            ? FVec3::UnitZ() : FVec3::UnitX();
        transformed_tangent = Cross(result.normal, reference);
    }
    result.tangent =
        Normalize3Or(transformed_tangent, FVec3::UnitX());
    result.bitangent = Normalize3Or(
        Cross(result.tangent, result.normal), FVec3::UnitZ());

    // Preserve the model's local +Z orientation when possible. This keeps
    // authored flow/ripple direction stable under reflected transforms.
    const FVec3 transformed_bitangent =
        TransformVector(FVec3::UnitZ(), model);
    if (IsFinite(transformed_bitangent)
        && Dot(result.bitangent, transformed_bitangent) < 0.0f) {
        result.bitangent = -result.bitangent;
    }
    return result;
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
    IAllocator& allocator = DefaultAllocator();
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

CWaterSurface3D::CWaterSurface3D() noexcept = default;

CWaterSurface3D::~CWaterSurface3D() noexcept {
    Shutdown();
}

EShaderStatus CWaterSurface3D::FCompiledShaders::Status() const noexcept {
    if (!vertex || !pixel) return EShaderStatus::Failed;
    const EShaderStatus vertex_status = vertex->Status();
    const EShaderStatus pixel_status = pixel->Status();
    if (vertex_status == EShaderStatus::Failed ||
        pixel_status == EShaderStatus::Failed) {
        return EShaderStatus::Failed;
    }
    if (vertex_status == EShaderStatus::Compiling ||
        pixel_status == EShaderStatus::Compiling) {
        return EShaderStatus::Compiling;
    }
    return EShaderStatus::Ready;
}

void CWaterSurface3D::SetParams(
    const FWaterSurface3DParams& params) noexcept {
    m_Params = SanitizeParams(params);
}

bool CWaterSurface3D::IsLocalXzSurfaceMesh(
    const AMeshAsset& mesh) noexcept {
    const TArray<FMeshVertex>& vertices = mesh.Vertices();
    const TArray<u32>& indices = mesh.Indices();
    if (vertices.Size() < 3u || indices.Size() < 3u ||
        indices.Size() % 3u != 0u) {
        return false;
    }

    f64 min_x = std::numeric_limits<f64>::max();
    f64 min_y = std::numeric_limits<f64>::max();
    f64 min_z = std::numeric_limits<f64>::max();
    f64 max_x = -std::numeric_limits<f64>::max();
    f64 max_y = -std::numeric_limits<f64>::max();
    f64 max_z = -std::numeric_limits<f64>::max();
    for (usize i = 0u; i < vertices.Size(); ++i) {
        const FMeshVertex& vertex = vertices[i];
        if (!IsFinite(vertex.position) || !IsFinite(vertex.normal)) {
            return false;
        }
        const f64 normal_length = std::hypot(
            static_cast<f64>(vertex.normal.x),
            static_cast<f64>(vertex.normal.y),
            static_cast<f64>(vertex.normal.z));
        if (!std::isfinite(normal_length) || normal_length <= 1e-6 ||
            std::abs(static_cast<f64>(vertex.normal.y)) /
                    normal_length < 0.5) {
            return false;
        }
        min_x = std::min(min_x, static_cast<f64>(vertex.position.x));
        min_y = std::min(min_y, static_cast<f64>(vertex.position.y));
        min_z = std::min(min_z, static_cast<f64>(vertex.position.z));
        max_x = std::max(max_x, static_cast<f64>(vertex.position.x));
        max_y = std::max(max_y, static_cast<f64>(vertex.position.y));
        max_z = std::max(max_z, static_cast<f64>(vertex.position.z));
    }

    const f64 horizontal_span =
        std::hypot(max_x - min_x, max_z - min_z);
    if (!std::isfinite(horizontal_span) || horizontal_span <= 1e-6) {
        return false;
    }
    const f64 local_plane_tolerance =
        std::max(1e-4, horizontal_span * 0.02);
    if (max_y - min_y > local_plane_tolerance) return false;

    f64 projected_twice_area = 0.0;
    for (usize i = 0u; i < indices.Size(); i += 3u) {
        const u32 ia = indices[i];
        const u32 ib = indices[i + 1u];
        const u32 ic = indices[i + 2u];
        if (ia >= vertices.Size() ||
            ib >= vertices.Size() ||
            ic >= vertices.Size()) {
            return false;
        }
        const FVec3& a = vertices[ia].position;
        const FVec3& b = vertices[ib].position;
        const FVec3& c = vertices[ic].position;
        const f64 ab_x = static_cast<f64>(b.x) - a.x;
        const f64 ab_z = static_cast<f64>(b.z) - a.z;
        const f64 ac_x = static_cast<f64>(c.x) - a.x;
        const f64 ac_z = static_cast<f64>(c.z) - a.z;
        projected_twice_area += std::abs(
            ab_x * ac_z - ab_z * ac_x);
    }
    const f64 minimum_area =
        std::max(1e-12, horizontal_span * horizontal_span * 1e-8);
    return std::isfinite(projected_twice_area) &&
           projected_twice_area > minimum_area;
}

TResult<void> CWaterSurface3D::Init(IRhiDevice& device, EFormat rt_format,
                                    EFormat depth_format,
                                    u32 msaa_samples) noexcept {
    FCompiledShaders compiled{};

    FShaderDesc vertex_description{};
    vertex_description.stage = EShaderStage::Vertex;
    vertex_description.hlsl_source = kWaterSurface3DHlsl;
    vertex_description.entry_point = "VSMain";
    vertex_description.debug_name = "WaterSurface3D.VS";
    auto vertex_result = CreateRhiShader(device, vertex_description);
    if (vertex_result.IsErr()) {
        return Err<void>(vertex_result.Error());
    }
    compiled.vertex = Move(vertex_result.Value());

    FShaderDesc pixel_description{};
    pixel_description.stage = EShaderStage::Pixel;
    pixel_description.hlsl_source = kWaterSurface3DHlsl;
    pixel_description.entry_point = "PSMain";
    pixel_description.debug_name = "WaterSurface3D.PS";
    auto pixel_result = CreateRhiShader(device, pixel_description);
    if (pixel_result.IsErr()) {
        return Err<void>(pixel_result.Error());
    }
    compiled.pixel = Move(pixel_result.Value());

    return InitWithCompiledShaders(
        device, Move(compiled),
        rt_format, depth_format, msaa_samples);
}

TResult<CWaterSurface3D::FCompiledShaders>
CWaterSurface3D::CompileShadersCpu() noexcept {
#if !WITH_RENDER_DILIGENT
    FShaderDesc vertex_description{};
    vertex_description.stage = EShaderStage::Vertex;
    vertex_description.hlsl_source = kWaterSurface3DHlsl;
    vertex_description.entry_point = "VSMain";
    vertex_description.debug_name = "WaterSurface3D.VS";

    auto vertex = MakeUnique<FDx12Shader>();
    if (!vertex) {
        return ACS_ERR(
            Memory, 711,
            "interactive-water vertex shader allocation failed");
    }
    const FHrResult vertex_result = vertex->Init(vertex_description);
    if (vertex_result.IsErr()) {
        return ACS_ERR_OS(
            Render, 712,
            "interactive-water vertex shader CPU compile failed",
            static_cast<u32>(vertex_result.hr));
    }

    FShaderDesc pixel_description{};
    pixel_description.stage = EShaderStage::Pixel;
    pixel_description.hlsl_source = kWaterSurface3DHlsl;
    pixel_description.entry_point = "PSMain";
    pixel_description.debug_name = "WaterSurface3D.PS";

    auto pixel = MakeUnique<FDx12Shader>();
    if (!pixel) {
        return ACS_ERR(
            Memory, 713,
            "interactive-water pixel shader allocation failed");
    }
    const FHrResult pixel_result = pixel->Init(pixel_description);
    if (pixel_result.IsErr()) {
        return ACS_ERR_OS(
            Render, 714,
            "interactive-water pixel shader CPU compile failed",
            static_cast<u32>(pixel_result.hr));
    }

    FCompiledShaders compiled{};
    compiled.vertex = TUniquePtr<IRhiShader>(
        vertex.Release(), vertex.GetAllocator());
    compiled.pixel = TUniquePtr<IRhiShader>(
        pixel.Release(), pixel.GetAllocator());
    return TResult<FCompiledShaders>(OkInit, Move(compiled));
#else
    return ACS_ERR(
        Render, 715,
        "interactive-water CPU compilation is available only on the raw "
        "DX12 backend");
#endif
}

TResult<CWaterSurface3D::FCompiledShaders>
CWaterSurface3D::BeginCompileShadersAsync(
    IRhiDevice& device) noexcept {
    if (!device.SupportsAsyncShaderCompilation()) {
        return ACS_ERR(
            Render, 362,
            "interactive-water backend-managed asynchronous "
            "compilation is unsupported");
    }
    FCompiledShaders compiled{};
    FShaderDesc vertex_description{};
    vertex_description.stage = EShaderStage::Vertex;
    vertex_description.hlsl_source = kWaterSurface3DHlsl;
    vertex_description.entry_point = "VSMain";
    vertex_description.debug_name = "WaterSurface3D.VS";
    vertex_description.compile_async = true;
    auto vertex_result =
        CreateRhiShader(device, vertex_description);
    if (vertex_result.IsErr()) {
        return Err<FCompiledShaders>(vertex_result.Error());
    }
    compiled.vertex = Move(vertex_result.Value());

    FShaderDesc pixel_description{};
    pixel_description.stage = EShaderStage::Pixel;
    pixel_description.hlsl_source = kWaterSurface3DHlsl;
    pixel_description.entry_point = "PSMain";
    pixel_description.debug_name = "WaterSurface3D.PS";
    pixel_description.compile_async = true;
    auto pixel_result =
        CreateRhiShader(device, pixel_description);
    if (pixel_result.IsErr()) {
        return Err<FCompiledShaders>(pixel_result.Error());
    }
    compiled.pixel = Move(pixel_result.Value());
    return TResult<FCompiledShaders>(OkInit, Move(compiled));
}

TResult<void> CWaterSurface3D::BeginInitWithCompiledShaders(
    IRhiDevice& device, FCompiledShaders&& shaders,
    EFormat rt_format, EFormat depth_format,
    u32 msaa_samples) noexcept {
    if (shaders.Status() != EShaderStatus::Ready) {
        return ACS_ERR(
            Render, 363,
            "interactive-water compiled shader set is not ready");
    }
    if (m_Pipeline || m_ManualDepthPipeline) {
        // Reinitialization is transactional: construct a complete replacement
        // away from the published renderer, then replace only after every PSO,
        // texture and constant buffer is known-good.
        CWaterSurface3D candidate;
        candidate.m_Params = m_Params;
        auto begin = candidate.BeginInitWithCompiledShaders(
            device, Move(shaders),
            rt_format, depth_format, msaa_samples);
        if (begin.IsErr()) return begin;
        auto complete =
            candidate.AdvanceInitialization(kConstantBufferRing);
        if (complete.IsErr()) {
            return Err<void>(complete.Error());
        }
        if (!complete.Value()) {
            return ACS_ERR(
                Render, 366,
                "interactive-water transactional replacement incomplete");
        }

        Shutdown();
        m_Device = candidate.m_Device;
        candidate.m_Device = nullptr;
        m_Vs = Move(candidate.m_Vs);
        m_Ps = Move(candidate.m_Ps);
        m_Pipeline = Move(candidate.m_Pipeline);
        m_ManualDepthPipeline =
            Move(candidate.m_ManualDepthPipeline);
        for (u32 i = 0u; i < kConstantBufferRing; ++i) {
            m_FrameCb[i] = Move(candidate.m_FrameCb[i]);
            m_ObjectCb[i] = Move(candidate.m_ObjectCb[i]);
        }
        m_NormalMap = Move(candidate.m_NormalMap);
        m_SceneFallback = Move(candidate.m_SceneFallback);
        m_FrameSlot = 0u;
        m_DrawCursor = 0u;
        m_DrawOverflowLogged = false;
        m_InitBufferCursor = kConstantBufferRing;
        m_InitializationPending = false;
        return Ok();
    }
    Shutdown();
    m_Device = &device;
    m_Vs = Move(shaders.vertex);
    m_Ps = Move(shaders.pixel);

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
    pipeline_description.texture_slots = 6;
    pipeline_description.texture_names[0] = "water_normal";
    pipeline_description.texture_names[1] = "authored_normal";
    pipeline_description.texture_names[2] = "scene_color";
    pipeline_description.texture_names[3] = "scene_depth";
    pipeline_description.texture_names[4] = "screen_reflection";
    pipeline_description.texture_names[5] = "shadow_map";
    pipeline_description.static_sampler_count = 6;
    pipeline_description.static_samplers[0].filter = ESamplerFilter::Linear;
    pipeline_description.static_samplers[0].address_u = ESamplerAddress::Wrap;
    pipeline_description.static_samplers[0].address_v = ESamplerAddress::Wrap;
    pipeline_description.static_samplers[1].filter = ESamplerFilter::Linear;
    pipeline_description.static_samplers[1].address_u = ESamplerAddress::Wrap;
    pipeline_description.static_samplers[1].address_v = ESamplerAddress::Wrap;
    pipeline_description.static_samplers[2].filter = ESamplerFilter::Linear;
    pipeline_description.static_samplers[2].address_u = ESamplerAddress::Clamp;
    pipeline_description.static_samplers[2].address_v = ESamplerAddress::Clamp;
    pipeline_description.static_samplers[3].filter = ESamplerFilter::Point;
    pipeline_description.static_samplers[3].address_u = ESamplerAddress::Clamp;
    pipeline_description.static_samplers[3].address_v = ESamplerAddress::Clamp;
    pipeline_description.static_samplers[4].filter = ESamplerFilter::Linear;
    pipeline_description.static_samplers[4].address_u = ESamplerAddress::Clamp;
    pipeline_description.static_samplers[4].address_v = ESamplerAddress::Clamp;
    pipeline_description.static_samplers[5].filter = ESamplerFilter::Point;
    pipeline_description.static_samplers[5].address_u = ESamplerAddress::Clamp;
    pipeline_description.static_samplers[5].address_v = ESamplerAddress::Clamp;

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
    m_InitBufferCursor = 0u;
    m_InitializationPending = true;
    m_Time = 0.0f;
    ClearDisturbances();
    return Ok();
}

TResult<bool> CWaterSurface3D::AdvanceInitialization(
    u32 buffer_pairs) noexcept {
    if (!m_InitializationPending) {
        return TResult<bool>(OkInit, m_Pipeline.Get() != nullptr);
    }
    if (m_Device == nullptr || buffer_pairs == 0u) {
        return ACS_ERR(
            Render, 364,
            "interactive-water bounded initialization has no device/work");
    }
    const u32 remaining =
        kConstantBufferRing - m_InitBufferCursor;
    const u32 count =
        buffer_pairs < remaining ? buffer_pairs : remaining;
    for (u32 offset = 0u; offset < count; ++offset) {
        const u32 index = m_InitBufferCursor + offset;
        FBufferDesc frame_description{};
        frame_description.size =
            ConstantBufferSize<FWaterFrameCb>();
        frame_description.usage = EBufferUsage::Uniform;
        frame_description.cpu_writable = true;
        auto frame_result =
            CreateRhiBuffer(*m_Device, frame_description);
        if (frame_result.IsErr()) {
            const auto error = frame_result.Error();
            Shutdown();
            return Err<bool>(error);
        }
        m_FrameCb[index] = Move(frame_result.Value());

        FBufferDesc object_description{};
        object_description.size =
            ConstantBufferSize<FWaterObjectCb>();
        object_description.usage = EBufferUsage::Uniform;
        object_description.cpu_writable = true;
        auto object_result =
            CreateRhiBuffer(*m_Device, object_description);
        if (object_result.IsErr()) {
            const auto error = object_result.Error();
            Shutdown();
            return Err<bool>(error);
        }
        m_ObjectCb[index] = Move(object_result.Value());
    }
    m_InitBufferCursor += count;
    if (m_InitBufferCursor >= kConstantBufferRing) {
        m_InitializationPending = false;
    }
    return TResult<bool>(
        OkInit, !m_InitializationPending);
}

TResult<void> CWaterSurface3D::InitWithCompiledShaders(
    IRhiDevice& device, FCompiledShaders&& shaders,
    EFormat rt_format, EFormat depth_format,
    u32 msaa_samples) noexcept {
    auto begin = BeginInitWithCompiledShaders(
        device, Move(shaders),
        rt_format, depth_format, msaa_samples);
    if (begin.IsErr()) return begin;
    auto advance =
        AdvanceInitialization(kConstantBufferRing);
    if (advance.IsErr()) return Err<void>(advance.Error());
    return advance.Value()
        ? Ok()
        : ACS_ERR(
            Render, 365,
            "interactive-water synchronous initialization incomplete");
}

void CWaterSurface3D::Shutdown() noexcept {
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
    m_InitBufferCursor = 0u;
    m_InitializationPending = false;
}

void CWaterSurface3D::Update(f32 dt) noexcept {
    if (dt < 0.0f) dt = 0.0f;
    if (!std::isfinite(dt)) return;
    const f64 next_time = static_cast<f64>(m_Time) + static_cast<f64>(dt);
    // The shader only needs a periodic phase clock. Wrapping every update also
    // keeps phase multiplication precise when a caller supplies a huge but
    // finite hitch delta; never expose FLT_MAX to sin/cos for even one frame.
    m_Time = static_cast<f32>(std::fmod(next_time, 65536.0));

    // Idle water advances its phase clock in O(1). This matters in editor
    // scenes where a prepared water renderer is currently off-screen or has
    // no interactions; scanning all 4096 reserved ownership slots would be
    // pure CPU workload and would not improve the visible result.
    u32 active_position = 0u;
    while (active_position < m_ActiveRippleCount) {
        const u32 storage_index =
            m_ActiveRippleStorageIndices[active_position];
        FRipple& ripple = m_Ripples[storage_index];
        const f64 next_age =
            static_cast<f64>(ripple.age) + static_cast<f64>(dt);
        if (next_age >= static_cast<f64>(ripple.lifetime)) {
            // EvaluateRippleAmplitudeScale reaches zero with zero slope and
            // curvature at this boundary. The slot can therefore be released
            // without removing a finite displacement/normal/foam contribution.
            ripple.age = ripple.lifetime;
            ripple.amplitude = 0.0f;
            DeactivateEventAtActivePosition(active_position);
            continue;
        }
        ripple.age = static_cast<f32>(next_age);
        if (ripple.age >= ripple.lifetime) {
            ripple.age = ripple.lifetime;
            ripple.amplitude = 0.0f;
            DeactivateEventAtActivePosition(active_position);
            continue;
        }
        const f32 amplitude_scale = EvaluateRippleAmplitudeScale(
            ripple.age, ripple.lifetime, ripple.damping);
        ripple.amplitude = ripple.initial_amplitude * amplitude_scale;
        if (amplitude_scale <= 0.0f) {
            DeactivateEventAtActivePosition(active_position);
            continue;
        }
        ++active_position;
    }
}

f32 CWaterSurface3D::EvaluateRippleAmplitudeScale(
    f32 age, f32 lifetime, f32 damping) noexcept {
    if (!std::isfinite(age) || !std::isfinite(lifetime)
        || !std::isfinite(damping) || lifetime <= 0.0f) {
        return 0.0f;
    }

    const f64 safe_age = age > 0.0f ? static_cast<f64>(age) : 0.0;
    const f64 normalized_age = safe_age / static_cast<f64>(lifetime);
    if (normalized_age >= 1.0) return 0.0f;

    // Preserve the existing physical exponential response for the first 65%
    // of the event. Over the final 35%, a quintic smootherstep fades to zero.
    // Its first and second derivatives are zero at both joins, so height,
    // analytic gradient (normal), and crest-energy (foam) remain coherent.
    constexpr f64 kFadeStart = 0.65;
    constexpr f64 kFadeDuration = 1.0 - kFadeStart;
    f64 tail = (normalized_age - kFadeStart) / kFadeDuration;
    if (tail < 0.0) tail = 0.0;
    if (tail > 1.0) tail = 1.0;
    const f64 smootherstep =
        tail * tail * tail * (tail * (tail * 6.0 - 15.0) + 10.0);
    const f64 lifetime_envelope = 1.0 - smootherstep;
    const f64 safe_damping = damping > 0.0f
        ? static_cast<f64>(damping) : 0.0;
    const f64 physical_damping = std::exp(-safe_damping * safe_age);
    const f64 scale = physical_damping * lifetime_envelope;
    return std::isfinite(scale) && scale > 0.0
        ? static_cast<f32>(scale) : 0.0f;
}

void CWaterSurface3D::DeactivateEventAtActivePosition(
    u32 active_position) noexcept {
    if (active_position >= m_ActiveRippleCount) return;
    const u32 storage_index =
        m_ActiveRippleStorageIndices[active_position];
    m_Ripples[storage_index] = FRipple{};

    --m_ActiveRippleCount;
    m_ActiveRippleStorageIndices[active_position] =
        m_ActiveRippleStorageIndices[m_ActiveRippleCount];
    m_ActiveRippleStorageIndices[m_ActiveRippleCount] = 0u;
}

bool CWaterSurface3D::AddEvent(u64 surface_id, bool wake,
                               FVec3 world_point, FVec3 direction,
                               f32 anisotropy, f32 radius,
                               f32 strength,
                               f32 initial_age) noexcept {
    if (!IsFinite(world_point) || !IsFinite(direction)
        || !std::isfinite(anisotropy) || !std::isfinite(radius)
        || !std::isfinite(strength) || !std::isfinite(initial_age)
        || std::abs(strength) < 1e-6f) {
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
    const f32 lifetime =
        m_Params.ripple_lifetime > 0.1f
            ? m_Params.ripple_lifetime : 0.1f;
    const f32 damping =
        m_Params.ripple_damping > 0.0f
            ? m_Params.ripple_damping : 0.0f;
    if (initial_age < 0.0f) initial_age = 0.0f;
    if (initial_age >= lifetime) return false;
    const f32 amplitude_scale =
        EvaluateRippleAmplitudeScale(
            initial_age, lifetime, damping);
    if (amplitude_scale <= 0.0f) return false;

    const u32 per_surface_limit =
        wake ? kWakeRippleSlots : kImpactRippleSlots;
    u32 matching_kind_count = 0u;
    bool surface_is_active = false;
    u64 active_surfaces[kMaxTrackedSurfaces]{};
    u32 active_surface_count = 0u;
    for (u32 active_position = 0u;
         active_position < m_ActiveRippleCount;
         ++active_position) {
        const FRipple& ripple = m_Ripples[
            m_ActiveRippleStorageIndices[active_position]];
        if (ripple.surface_id == surface_id) {
            surface_is_active = true;
            if (ripple.wake == wake) ++matching_kind_count;
        }
        bool known_surface = false;
        for (u32 s = 0u; s < active_surface_count; ++s) {
            if (active_surfaces[s] == ripple.surface_id) {
                known_surface = true;
                break;
            }
        }
        if (!known_surface && active_surface_count < kMaxTrackedSurfaces) {
            active_surfaces[active_surface_count++] = ripple.surface_id;
        }
    }
    u32 free_slot = kMaxStoredRipples;
    if (m_ActiveRippleCount < kMaxStoredRipples) {
        for (u32 i = 0u; i < kMaxStoredRipples; ++i) {
            if (!m_Ripples[i].active) {
                free_slot = i;
                break;
            }
        }
    }
    if (matching_kind_count >= per_surface_limit ||
        free_slot == kMaxStoredRipples ||
        (!surface_is_active &&
         active_surface_count >= kMaxTrackedSurfaces)) {
        // Persistence is deliberate: never replace an active visible event.
        return false;
    }

    FRipple& ripple = m_Ripples[free_slot];
    ripple.center = world_point;
    ripple.direction = Normalize3(direction);
    ripple.initial_radius = radius;
    ripple.initial_amplitude = strength;
    ripple.amplitude = strength * amplitude_scale;
    ripple.age = initial_age;
    ripple.speed =
        m_Params.ripple_speed > 0.0f ? m_Params.ripple_speed : 0.0f;
    ripple.lifetime = lifetime;
    ripple.damping = damping;
    ripple.anisotropy = anisotropy;
    ripple.surface_id = surface_id;
    ripple.wake = wake;
    ripple.active = true;
    m_ActiveRippleStorageIndices[m_ActiveRippleCount++] = free_slot;
    return true;
}

u32 CWaterSurface3D::AvailableEventSlots(
    u64 surface_id, bool wake) const noexcept {
    const u32 per_surface_limit =
        wake ? kWakeRippleSlots : kImpactRippleSlots;
    u32 matching_kind_count = 0u;
    bool surface_is_active = false;
    u64 active_surfaces[kMaxTrackedSurfaces]{};
    u32 active_surface_count = 0u;

    for (u32 active_position = 0u;
         active_position < m_ActiveRippleCount;
         ++active_position) {
        const FRipple& ripple = m_Ripples[
            m_ActiveRippleStorageIndices[active_position]];
        if (ripple.surface_id == surface_id) {
            surface_is_active = true;
            if (ripple.wake == wake) ++matching_kind_count;
        }
        bool known_surface = false;
        for (u32 surface = 0u;
             surface < active_surface_count; ++surface) {
            if (active_surfaces[surface] == ripple.surface_id) {
                known_surface = true;
                break;
            }
        }
        if (!known_surface &&
            active_surface_count < kMaxTrackedSurfaces) {
            active_surfaces[active_surface_count++] =
                ripple.surface_id;
        }
    }

    const u32 free_slot_count =
        kMaxStoredRipples - m_ActiveRippleCount;
    if (matching_kind_count >= per_surface_limit ||
        free_slot_count == 0u ||
        (!surface_is_active &&
         active_surface_count >= kMaxTrackedSurfaces)) {
        return 0u;
    }
    const u32 per_surface_available =
        per_surface_limit - matching_kind_count;
    return std::min(per_surface_available, free_slot_count);
}

bool CWaterSurface3D::AddDisturbance(FVec3 world_point, f32 radius,
                                     f32 strength) noexcept {
    return AddDisturbanceForSurface(
        0u, world_point, radius, strength);
}

bool CWaterSurface3D::AddDisturbanceForSurface(
    u64 surface_id, FVec3 world_point,
    f32 radius, f32 strength) noexcept {
    return AddEvent(surface_id, false, world_point,
                    FVec3{1.0f, 0.0f, 0.0f}, 1.0f,
                    radius, strength);
}

bool CWaterSurface3D::AddWake(FVec3 world_point, FVec3 world_velocity,
                              f32 radius, f32 strength) noexcept {
    return AddWakeForSurface(
        0u, world_point, world_velocity, radius, strength);
}

bool CWaterSurface3D::AddWakeForSurface(
    u64 surface_id, FVec3 world_point, FVec3 world_velocity,
    f32 radius, f32 strength) noexcept {
    return AddWakeEventForSurface(
        surface_id, world_point, world_velocity,
        radius, strength, 0.0f);
}

bool CWaterSurface3D::AddWakeEventForSurface(
    u64 surface_id, FVec3 world_point, FVec3 world_velocity,
    f32 radius, f32 strength, f32 initial_age) noexcept {
    if (!IsFinite(world_velocity)) return false;
    const f64 speed64 = std::hypot(
        static_cast<f64>(world_velocity.x),
        static_cast<f64>(world_velocity.y),
        static_cast<f64>(world_velocity.z));
    if (!std::isfinite(speed64)) return false;
    const f32 speed = speed64 > 65504.0
        ? 65504.0f : static_cast<f32>(speed64);
    const FVec3 direction = speed < 1e-4f
        ? FVec3{1.0f, 0.0f, 0.0f}
        : Normalize3(world_velocity);
    const f32 anisotropy =
        1.35f + (speed < 8.0f ? speed * 0.16f : 1.28f);
    const FVec3 trailing_point{
        world_point.x - direction.x * radius * 0.38f,
        world_point.y - direction.y * radius * 0.38f,
        world_point.z - direction.z * radius * 0.38f,
    };
    if (!IsFinite(trailing_point)) return false;
    return AddEvent(surface_id, true, trailing_point, direction,
                    anisotropy, radius, strength, initial_age);
}

u32 CWaterSurface3D::AddWakeSegment(
    FVec3 segment_start, FVec3 segment_end,
    f32 duration, f32 sample_spacing,
    f32 radius, f32 strength) noexcept {
    return AddWakeSegmentForSurface(
        0u, segment_start, segment_end,
        duration, sample_spacing, radius, strength);
}

u32 CWaterSurface3D::AddWakeSegmentForSurface(
    u64 surface_id,
    FVec3 segment_start, FVec3 segment_end,
    f32 duration, f32 sample_spacing,
    f32 radius, f32 strength) noexcept {
    if (!IsFinite(segment_start) || !IsFinite(segment_end) ||
        !std::isfinite(duration) || duration <= 0.0f ||
        !std::isfinite(sample_spacing) || sample_spacing <= 0.0f ||
        !std::isfinite(radius) || !std::isfinite(strength) ||
        std::abs(strength) < 1e-6f) {
        return 0u;
    }

    const f64 delta_x =
        static_cast<f64>(segment_end.x) - segment_start.x;
    const f64 delta_y =
        static_cast<f64>(segment_end.y) - segment_start.y;
    const f64 delta_z =
        static_cast<f64>(segment_end.z) - segment_start.z;
    const f64 distance =
        std::hypot(delta_x, delta_y, delta_z);
    if (!std::isfinite(distance) || distance <= 1e-6) return 0u;

    const u32 available =
        AvailableEventSlots(surface_id, true);
    if (available == 0u) return 0u;

    const f64 lifetime = static_cast<f64>(
        m_Params.ripple_lifetime > 0.1f
            ? m_Params.ripple_lifetime : 0.1f);
    const f64 visible_duration = std::min(
        static_cast<f64>(duration), lifetime);
    const f64 visible_fraction =
        visible_duration / static_cast<f64>(duration);
    const f64 visible_distance = distance * visible_fraction;
    const f64 requested_samples =
        std::ceil(visible_distance /
                  static_cast<f64>(sample_spacing));
    u32 sample_count = available;
    if (std::isfinite(requested_samples) &&
        requested_samples < static_cast<f64>(available)) {
        sample_count = std::max(
            1u, static_cast<u32>(requested_samples));
    }

    const f64 inverse_distance = 1.0 / distance;
    const f64 physical_speed = std::min(
        distance / static_cast<f64>(duration), 65504.0);
    const FVec3 velocity{
        static_cast<f32>(
            delta_x * inverse_distance * physical_speed),
        static_cast<f32>(
            delta_y * inverse_distance * physical_speed),
        static_cast<f32>(
            delta_z * inverse_distance * physical_speed),
    };

    u32 accepted = 0u;
    for (u32 sample = 1u; sample <= sample_count; ++sample) {
        const f64 visible_t =
            static_cast<f64>(sample) /
            static_cast<f64>(sample_count);
        const f64 age =
            visible_duration * (1.0 - visible_t);
        const f64 t =
            1.0 - visible_fraction * (1.0 - visible_t);
        const FVec3 point{
            static_cast<f32>(
                static_cast<f64>(segment_start.x) + delta_x * t),
            static_cast<f32>(
                static_cast<f64>(segment_start.y) + delta_y * t),
            static_cast<f32>(
                static_cast<f64>(segment_start.z) + delta_z * t),
        };
        if (!AddWakeEventForSurface(
                surface_id, point, velocity,
                radius, strength,
                static_cast<f32>(age))) {
            continue;
        }
        ++accepted;
    }
    return accepted;
}

void CWaterSurface3D::ClearDisturbances() noexcept {
    for (u32 i = 0; i < kMaxStoredRipples; ++i) {
        m_Ripples[i] = FRipple{};
        m_ActiveRippleStorageIndices[i] = 0u;
    }
    m_ActiveRippleCount = 0u;
}

void CWaterSurface3D::ClearDisturbancesForSurface(
    u64 surface_id) noexcept {
    u32 active_position = 0u;
    while (active_position < m_ActiveRippleCount) {
        const FRipple& ripple = m_Ripples[
            m_ActiveRippleStorageIndices[active_position]];
        if (ripple.surface_id == surface_id) {
            DeactivateEventAtActivePosition(active_position);
            continue;
        }
        ++active_position;
    }
}

u32 CWaterSurface3D::ActiveRippleCount() const noexcept {
    return m_ActiveRippleCount;
}

u32 CWaterSurface3D::ActiveRippleCountForSurface(
    u64 surface_id) const noexcept {
    u32 count = 0u;
    for (u32 active_position = 0u;
         active_position < m_ActiveRippleCount;
         ++active_position) {
        const FRipple& ripple = m_Ripples[
            m_ActiveRippleStorageIndices[active_position]];
        if (ripple.surface_id == surface_id) {
            ++count;
        }
    }
    return count;
}

f32 CWaterSurface3D::ConservativeDisplacementBoundForSurface(
    u64 surface_id,
    const FWaterSurface3DParams& params) const noexcept {
    // EvaluateAmbientWaves uses these four normalized layer weights. Since
    // abs(sin) <= 1, their sum is the exact phase-independent height bound.
    constexpr f64 kAmbientAmplitudeWeightSum =
        0.52 + 0.27 + 0.145 + 0.085;
    const FWaterSurface3DParams safe = SanitizeParams(params);
    f64 bound =
        kAmbientAmplitudeWeightSum *
        static_cast<f64>(safe.wave_amplitude);

    // DrawMesh uploads at most kMaxRipples for this surface from the same dense
    // active list. Each packet envelope and wake mask is at most one, so the
    // sum of absolute current amplitudes is conservative without imposing a
    // scene-wide fixed inflation.
    u32 uploaded = 0u;
    for (u32 active_position = 0u;
         active_position < m_ActiveRippleCount &&
         uploaded < kMaxRipples;
         ++active_position) {
        const FRipple& ripple = m_Ripples[
            m_ActiveRippleStorageIndices[active_position]];
        if (ripple.surface_id != surface_id)
            continue;
        if (!std::isfinite(ripple.amplitude))
            return std::numeric_limits<f32>::quiet_NaN();
        bound += std::abs(static_cast<f64>(ripple.amplitude));
        ++uploaded;
    }
    return std::isfinite(bound) &&
           bound <= static_cast<f64>(
               std::numeric_limits<f32>::max())
        ? static_cast<f32>(bound)
        : std::numeric_limits<f32>::quiet_NaN();
}

void CWaterSurface3D::SetFrame(const FMat4& view_projection,
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

void CWaterSurface3D::SetEnvironment(
    FVec3 zenith, FVec3 horizon, FVec3 ground) noexcept {
    m_EnvironmentZenith = ClampFinite(
        zenith, m_EnvironmentZenith, 0.0f, 65504.0f);
    m_EnvironmentHorizon = ClampFinite(
        horizon, m_EnvironmentHorizon, 0.0f, 65504.0f);
    m_EnvironmentGround = ClampFinite(
        ground, m_EnvironmentGround, 0.0f, 65504.0f);
}

void CWaterSurface3D::SetShadowMap(
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

void CWaterSurface3D::DrawMesh(IRhiCommandList& command_list,
                               const FGpuMesh& mesh,
                               const FMat4& model,
                               IRhiTexture* scene_color,
                               IRhiTexture* scene_depth,
                               IRhiTexture* screen_reflection,
                               u64 surface_id,
                               bool hardware_depth_bound,
                               IRhiTexture* authored_normal_map,
                               f32 authored_normal_strength) noexcept {
    if (m_InitializationPending ||
        !m_Pipeline || !m_ManualDepthPipeline || !IsFinite(model)
        || !mesh.vertex_buffer || !mesh.index_buffer
        || !m_NormalMap || !m_SceneFallback) {
        return;
    }

    if (m_DrawCursor >= kMaxDrawsPerFrame) {
        if (!m_DrawOverflowLogged) {
            ACS_LOG_WARN(
                "CWaterSurface3D: more than %u DrawMesh calls after one "
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

    const FWaterSurfaceFrame surface =
        BuildWaterSurfaceFrame(model);
    u32 ripple_count = 0;
    for (u32 active_position = 0u;
         active_position < m_ActiveRippleCount &&
         ripple_count < kMaxRipples;
         ++active_position) {
        const FRipple& ripple = m_Ripples[
            m_ActiveRippleStorageIndices[active_position]];
        if (ripple.surface_id != surface_id) continue;
        const f32 remaining =
            1.0f - ripple.age / (ripple.lifetime > 0.0f
                                    ? ripple.lifetime : 1.0f);
        const FVec3 relative_center = ripple.center - surface.origin;
        const FVec2 surface_center{
            Dot(relative_center, surface.tangent),
            Dot(relative_center, surface.bitangent),
        };
        FVec2 surface_direction{
            Dot(ripple.direction, surface.tangent),
            Dot(ripple.direction, surface.bitangent),
        };
        const f32 projected_direction_length =
            std::hypot(surface_direction.x, surface_direction.y);
        const bool has_surface_direction =
            std::isfinite(projected_direction_length)
            && projected_direction_length > 1e-5f;
        surface_direction = has_surface_direction
            ? Normalize2(surface_direction) : FVec2{1.0f, 0.0f};
        frame.ripple_a[ripple_count] =
            FVec4{surface_center.x, surface_center.y,
                  ripple.initial_radius + ripple.speed * ripple.age,
                  ripple.amplitude};
        frame.ripple_b[ripple_count] =
            FVec4{surface_direction.x, surface_direction.y,
                  has_surface_direction ? ripple.anisotropy : 1.0f,
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
    frame.author_normal_params =
        FVec4{
            authored_normal_map ? 1.0f : 0.0f,
            ClampFinite(
                authored_normal_strength, 1.0f, 0.0f, 8.0f),
            0.0f, 0.0f};
    const f32 shadow_texel_size =
        m_ShadowMap && m_ShadowMap->Width() > 0
        ? 1.0f / static_cast<f32>(m_ShadowMap->Width())
        : 0.0f;
    frame.shadow_params =
        FVec4{m_ShadowMap ? 1.0f : 0.0f,
              m_ShadowBias, shadow_texel_size, m_ShadowPcfRadius};
    frame.environment_zenith =
        FVec4{m_EnvironmentZenith, 1.0f};
    frame.environment_horizon =
        FVec4{m_EnvironmentHorizon, 1.0f};
    frame.environment_ground =
        FVec4{m_EnvironmentGround, 1.0f};
    frame_buffer->Update(&frame, sizeof(frame));

    FWaterObjectCb object{};
    object.model = model;
    const FMat4& normal_matrix = surface.normal_matrix;
    object.normal_row0 =
        FVec4{normal_matrix.m[0][0], normal_matrix.m[0][1],
              normal_matrix.m[0][2], 0.0f};
    object.normal_row1 =
        FVec4{normal_matrix.m[1][0], normal_matrix.m[1][1],
              normal_matrix.m[1][2], 0.0f};
    object.normal_row2 =
        FVec4{normal_matrix.m[2][0], normal_matrix.m[2][1],
              normal_matrix.m[2][2], 0.0f};
    object.surface_origin = FVec4{surface.origin, 1.0f};
    object.surface_tangent = FVec4{surface.tangent, 0.0f};
    object.surface_bitangent = FVec4{surface.bitangent, 0.0f};
    object_buffer->Update(&object, sizeof(object));

    IRhiPipeline* pipeline =
        scene_depth && !hardware_depth_bound
            ? m_ManualDepthPipeline.Get()
            : m_Pipeline.Get();
    command_list.SetPipeline(*pipeline);
    command_list.SetConstantBuffer(0, *frame_buffer);
    command_list.SetConstantBuffer(1, *object_buffer);
    command_list.SetTexture(0, *m_NormalMap);
    command_list.SetTexture(
        1, authored_normal_map
            ? *authored_normal_map : *m_NormalMap);
    command_list.SetTexture(
        2, scene_color ? *scene_color : *m_SceneFallback);
    command_list.SetTexture(
        3, scene_depth ? *scene_depth : *m_SceneFallback);
    command_list.SetTexture(
        4, screen_reflection
            ? *screen_reflection : *m_SceneFallback);
    command_list.SetTexture(
        5, m_ShadowMap ? *m_ShadowMap : *m_SceneFallback);
    command_list.SetVertexBuffer(*mesh.vertex_buffer, mesh.vertex_stride);
    command_list.SetIndexBuffer(*mesh.index_buffer);
    command_list.DrawIndexed(mesh.index_count);
}

} // namespace acs
