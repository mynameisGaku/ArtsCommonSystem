// SPDX-License-Identifier: Apache-2.0
#include "render/SubsurfaceScattering.h"

#include "foundation/Move.h"
#if !WITH_RENDER_DILIGENT
#include "render/Dx12/Dx12Shader.h"
#endif

#include <cmath>

namespace acs {

namespace {

f32 ClampFinite(f32 value, f32 fallback, f32 minimum,
                f32 maximum) noexcept {
    if (!std::isfinite(value)) value = fallback;
    if (value < minimum) value = minimum;
    if (value > maximum) value = maximum;
    return value;
}

struct alignas(16) FSsssCbLayout {
    FMat4 inverse_view_projection;
    FVec4 pass_data;   // xy=inv resolution, zw=axis selector
    FVec4 diffusion;   // x=world radius, y=strength, z=depth sigma, w=normal power
    FVec4 profile;     // xyz=relative RGB radii, w=max radius in pixels
};

constexpr usize ConstantBufferSize() noexcept {
    return (sizeof(FSsssCbLayout) + 255u) & ~usize(255u);
}

const char* kSubsurfaceScatteringHlsl = R"(
#pragma pack_matrix(row_major)

cbuffer SsssCB : register(b0) {
    float4x4 inverse_view_projection;
    float4 pass_data; // xy=inv resolution, zw=axis selector
    float4 diffusion; // x=world radius, y=strength, z=depth sigma, w=normal power
    float4 profile;   // xyz=relative RGB radii, w=max radius in pixels
};

Texture2D diffuse_input   : register(t0);
Texture2D scene_depth     : register(t1);
Texture2D normal_gbuffer  : register(t2);
Texture2D material_data   : register(t3);
Texture2D original_diffuse : register(t4);
Texture2D scene_color      : register(t5);

SamplerState diffuse_input_sampler    : register(s0);
SamplerState scene_depth_sampler      : register(s1);
SamplerState normal_gbuffer_sampler   : register(s2);
SamplerState material_data_sampler    : register(s3);
SamplerState original_diffuse_sampler : register(s4);
SamplerState scene_color_sampler      : register(s5);

struct VSOut {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vertex_id : SV_VertexID) {
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    VSOut output;
    output.uv = uv;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.position.y = -output.position.y;
    return output;
}

float2 ClampUv(float2 uv) {
    return clamp(uv, pass_data.xy * 0.5, 1.0 - pass_data.xy * 0.5);
}

float3 SafeHdr(float3 color) {
    return all(abs(color) < 1.0e30) ? max(color, 0.0) : 0.0;
}

float3 ReconstructWorldPosition(float2 uv, float depth) {
    float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    clip.y = -clip.y;
    float4 world = mul(clip, inverse_view_projection);
    float safe_w = abs(world.w) > 1e-6
        ? world.w
        : (world.w < 0.0 ? -1e-6 : 1e-6);
    return world.xyz / safe_w;
}

float3 LoadNormal(float2 uv) {
    float3 normal =
        normal_gbuffer.SampleLevel(normal_gbuffer_sampler, ClampUv(uv), 0).xyz;
    float length_squared = dot(normal, normal);
    return length_squared > 1e-8
        ? normal * rsqrt(length_squared)
        : float3(0.0, 0.0, 0.0);
}

float Max3(float3 value) {
    return max(value.x, max(value.y, value.z));
}

float3 ResolveProfileRadii(float4 material) {
    float3 authored = max(material.rgb, 0.0.xxx);
    if (Max3(authored) > 1e-7) return authored;

    // Compatibility for custom producers written against the old scalar
    // contract. The engine PBR MRT always supplies authored RGB world radii.
    return max(
        diffusion.x * max(profile.xyz, 0.05.xxx),
        1e-6.xxx);
}

float3 ProfileWeight(float world_offset, float3 radii) {
    // Radius is the physically authored support distance. The Gaussian has
    // fallen to roughly 4.4% at that distance and every colour channel is
    // normalized independently after the gather.
    float3 normalized_offset =
        world_offset / max(radii, 1e-6.xxx);
    return exp2(
        -4.5 * normalized_offset * normalized_offset);
}

float ProfileSimilarity(float3 center_radii, float3 sample_radii) {
    float3 denominator =
        max(max(center_radii, sample_radii), 1e-5.xxx);
    float3 relative_delta =
        abs(sample_radii - center_radii) / denominator;
    float maximum_delta = Max3(relative_delta);
    if (maximum_delta > 0.25) return 0.0;
    return exp2(-64.0 * maximum_delta * maximum_delta);
}

float4 BlurDiffuse(float2 center_uv) {
    center_uv = ClampUv(center_uv);
    float4 center_diffuse =
        diffuse_input.SampleLevel(diffuse_input_sampler, center_uv, 0);
    center_diffuse.rgb = SafeHdr(center_diffuse.rgb);

    float center_depth =
        scene_depth.SampleLevel(scene_depth_sampler, center_uv, 0).r;
    float4 center_material =
        material_data.SampleLevel(material_data_sampler, center_uv, 0);
    float coverage = saturate(center_material.a);
    if (center_depth >= 0.9999 || coverage <= 1e-4 ||
        diffusion.y <= 1e-4) {
        return center_diffuse;
    }
    float3 center_radii = ResolveProfileRadii(center_material);
    float maximum_world_radius = Max3(center_radii);
    if (maximum_world_radius <= 1e-7) return center_diffuse;

    float3 center_normal = LoadNormal(center_uv);
    if (dot(center_normal, center_normal) < 0.5) return center_diffuse;

    float3 center_position =
        ReconstructWorldPosition(center_uv, center_depth);
    float2 axis_texel = pass_data.xy * pass_data.zw;
    float3 adjacent_position = ReconstructWorldPosition(
        ClampUv(center_uv + axis_texel), center_depth);
    float world_per_pixel =
        max(length(adjacent_position - center_position), 1e-6);

    float pixel_radius = clamp(
        maximum_world_radius / world_per_pixel,
        0.0, max(profile.w, 1.0));
    if (pixel_radius < 0.25) return center_diffuse;

    float3 accumulated = center_diffuse.rgb;
    float3 normalization = 1.0;

    static const float kOffsets[6] = {
        0.12, 0.25, 0.40, 0.58, 0.78, 1.00
    };

    [unroll]
    for (int tap = 0; tap < 6; ++tap) {
        float normalized_offset = kOffsets[tap];
        float sample_world_offset =
            maximum_world_radius * normalized_offset;

        [unroll]
        for (int side = 0; side < 2; ++side) {
            float sign_value = side == 0 ? -1.0 : 1.0;
            float2 sample_uv = ClampUv(
                center_uv + axis_texel *
                (pixel_radius * normalized_offset * sign_value));
            float sample_depth =
                scene_depth.SampleLevel(scene_depth_sampler, sample_uv, 0).r;
            if (sample_depth >= 0.9999) continue;

            float4 sample_material =
                material_data.SampleLevel(material_data_sampler, sample_uv, 0);
            float sample_coverage = saturate(sample_material.a);
            if (sample_coverage <= 1e-4) continue;

            float3 sample_radii =
                ResolveProfileRadii(sample_material);
            float profile_similarity =
                ProfileSimilarity(center_radii, sample_radii);
            if (profile_similarity <= 1e-4) continue;

            float3 sample_normal = LoadNormal(sample_uv);
            if (dot(sample_normal, sample_normal) < 0.5) continue;
            float normal_weight = pow(
                saturate(dot(center_normal, sample_normal)),
                max(diffusion.w, 1.0));

            float3 sample_position =
                ReconstructWorldPosition(sample_uv, sample_depth);
            float3 separation = sample_position - center_position;
            float plane_delta = max(
                abs(dot(separation, center_normal)),
                abs(dot(separation, sample_normal)));
            float depth_tolerance =
                max(diffusion.z,
                    max(maximum_world_radius * 0.08, 1e-6));
            float depth_ratio = plane_delta / depth_tolerance;
            float depth_weight = exp2(-depth_ratio * depth_ratio);

            float bilateral_weight =
                sample_coverage * profile_similarity *
                normal_weight * depth_weight;
            // Use the centre/sample mean profile so neighbouring authored
            // values remain reciprocal while still preserving hard material
            // boundaries through ProfileSimilarity.
            float3 pair_radii =
                max((center_radii + sample_radii) * 0.5, 1e-6.xxx);
            float3 radial_weight =
                ProfileWeight(sample_world_offset, pair_radii);
            float3 weight = radial_weight * bilateral_weight;
            float3 sample_diffuse = SafeHdr(
                diffuse_input.SampleLevel(
                    diffuse_input_sampler, sample_uv, 0).rgb);
            accumulated += sample_diffuse * weight;
            normalization += weight;
        }
    }

    // Per-channel normalization is the energy-stability contract: a constant
    // diffuse field remains exactly constant for every profile/radius.
    float3 result = accumulated / max(normalization, 1e-5);
    return float4(SafeHdr(result), center_diffuse.a);
}

float4 PSBlur(VSOut input) : SV_TARGET {
    return BlurDiffuse(input.uv);
}

float4 PSComposite(VSOut input) : SV_TARGET {
    float2 uv = ClampUv(input.uv);
    float4 scene = scene_color.SampleLevel(scene_color_sampler, uv, 0);
    scene.rgb = SafeHdr(scene.rgb);
    float3 original = SafeHdr(
        original_diffuse.SampleLevel(original_diffuse_sampler, uv, 0).rgb);
    float3 blurred = BlurDiffuse(uv).rgb;
    float coverage = saturate(
        material_data.SampleLevel(material_data_sampler, uv, 0).a);

    // Only replace the diffuse term. Specular, clear-coat and emissive energy
    // in scene_color are untouched by the screen-space diffusion.
    float mix_strength = saturate(diffusion.y) * coverage;
    float3 composited =
        scene.rgb + (blurred - original) * mix_strength;
    return float4(SafeHdr(composited), scene.a);
}
)";

FSamplerDesc LinearClampSampler() noexcept {
    FSamplerDesc sampler{};
    sampler.filter = ESamplerFilter::Linear;
    sampler.address_u = ESamplerAddress::Clamp;
    sampler.address_v = ESamplerAddress::Clamp;
    return sampler;
}

FSamplerDesc PointClampSampler() noexcept {
    FSamplerDesc sampler{};
    sampler.filter = ESamplerFilter::Point;
    sampler.address_u = ESamplerAddress::Clamp;
    sampler.address_v = ESamplerAddress::Clamp;
    return sampler;
}

} // namespace

EShaderStatus
FSubsurfaceScattering::FCompiledShaders::Status() const noexcept {
    if (!vertex || !blur_pixel || !composite_pixel) {
        return EShaderStatus::Failed;
    }
    const EShaderStatus vertex_status = vertex->Status();
    const EShaderStatus blur_status = blur_pixel->Status();
    const EShaderStatus composite_status = composite_pixel->Status();
    if (vertex_status == EShaderStatus::Failed ||
        blur_status == EShaderStatus::Failed ||
        composite_status == EShaderStatus::Failed) {
        return EShaderStatus::Failed;
    }
    if (vertex_status == EShaderStatus::Compiling ||
        blur_status == EShaderStatus::Compiling ||
        composite_status == EShaderStatus::Compiling) {
        return EShaderStatus::Compiling;
    }
    return EShaderStatus::Ready;
}

void FSubsurfaceScatteringParams::Sanitize() noexcept {
    radius_world =
        ClampFinite(radius_world, 0.012f, 0.0f, 1000.0f);
    channel_radius = FVec3{
        ClampFinite(channel_radius.x, 1.0f, 0.05f, 4.0f),
        ClampFinite(channel_radius.y, 0.55f, 0.05f, 4.0f),
        ClampFinite(channel_radius.z, 0.25f, 0.05f, 4.0f),
    };
    strength = ClampFinite(strength, 1.0f, 0.0f, 1.0f);
    depth_sigma =
        ClampFinite(depth_sigma, 0.001f, 1e-6f, 1000.0f);
    normal_power =
        ClampFinite(normal_power, 24.0f, 1.0f, 128.0f);
    max_radius_pixels =
        ClampFinite(max_radius_pixels, 64.0f, 1.0f, 128.0f);
}

TResult<void> FSubsurfaceScattering::Init(
    IRhiDevice& device, u32 width, u32 height) noexcept {
    FCompiledShaders shaders{};
    FShaderDesc vertex_description{};
    vertex_description.stage = EShaderStage::Vertex;
    vertex_description.hlsl_source = kSubsurfaceScatteringHlsl;
    vertex_description.entry_point = "VSMain";
    vertex_description.debug_name = "SubsurfaceScattering.VS";
    auto vertex_result = CreateRhiShader(device, vertex_description);
    if (vertex_result.IsErr()) return Err<void>(vertex_result.Error());
    shaders.vertex = Move(vertex_result.Value());

    FShaderDesc blur_description{};
    blur_description.stage = EShaderStage::Pixel;
    blur_description.hlsl_source = kSubsurfaceScatteringHlsl;
    blur_description.entry_point = "PSBlur";
    blur_description.debug_name = "SubsurfaceScattering.BlurPS";
    auto blur_result = CreateRhiShader(device, blur_description);
    if (blur_result.IsErr()) return Err<void>(blur_result.Error());
    shaders.blur_pixel = Move(blur_result.Value());

    FShaderDesc composite_description{};
    composite_description.stage = EShaderStage::Pixel;
    composite_description.hlsl_source = kSubsurfaceScatteringHlsl;
    composite_description.entry_point = "PSComposite";
    composite_description.debug_name = "SubsurfaceScattering.CompositePS";
    auto composite_result =
        CreateRhiShader(device, composite_description);
    if (composite_result.IsErr()) {
        return Err<void>(composite_result.Error());
    }
    shaders.composite_pixel = Move(composite_result.Value());
    return InitWithCompiledShaders(
        device, Move(shaders), width, height);
}

TResult<FSubsurfaceScattering::FCompiledShaders>
FSubsurfaceScattering::CompileShadersCpu() noexcept {
#if !WITH_RENDER_DILIGENT
    FCompiledShaders shaders{};

    FShaderDesc vertex_description{};
    vertex_description.stage = EShaderStage::Vertex;
    vertex_description.hlsl_source = kSubsurfaceScatteringHlsl;
    vertex_description.entry_point = "VSMain";
    vertex_description.debug_name = "SubsurfaceScattering.VS";
    auto vertex = MakeUnique<FDx12Shader>();
    if (!vertex) {
        return ACS_ERR(
            Memory, 353, "SSSS vertex shader allocation failed");
    }
    const FHrResult vertex_result = vertex->Init(vertex_description);
    if (vertex_result.IsErr()) {
        return ACS_ERR_OS(
            Render, 354, "SSSS vertex shader CPU compile failed",
            static_cast<u32>(vertex_result.hr));
    }
    shaders.vertex = Move(vertex);

    FShaderDesc blur_description{};
    blur_description.stage = EShaderStage::Pixel;
    blur_description.hlsl_source = kSubsurfaceScatteringHlsl;
    blur_description.entry_point = "PSBlur";
    blur_description.debug_name = "SubsurfaceScattering.BlurPS";
    auto blur = MakeUnique<FDx12Shader>();
    if (!blur) {
        return ACS_ERR(
            Memory, 355, "SSSS blur shader allocation failed");
    }
    const FHrResult blur_result = blur->Init(blur_description);
    if (blur_result.IsErr()) {
        return ACS_ERR_OS(
            Render, 356, "SSSS blur shader CPU compile failed",
            static_cast<u32>(blur_result.hr));
    }
    shaders.blur_pixel = Move(blur);

    FShaderDesc composite_description{};
    composite_description.stage = EShaderStage::Pixel;
    composite_description.hlsl_source = kSubsurfaceScatteringHlsl;
    composite_description.entry_point = "PSComposite";
    composite_description.debug_name = "SubsurfaceScattering.CompositePS";
    auto composite = MakeUnique<FDx12Shader>();
    if (!composite) {
        return ACS_ERR(
            Memory, 357, "SSSS composite shader allocation failed");
    }
    const FHrResult composite_result =
        composite->Init(composite_description);
    if (composite_result.IsErr()) {
        return ACS_ERR_OS(
            Render, 358, "SSSS composite shader CPU compile failed",
            static_cast<u32>(composite_result.hr));
    }
    shaders.composite_pixel = Move(composite);
    return TResult<FCompiledShaders>(OkInit, Move(shaders));
#else
    return ACS_ERR(
        Render, 359,
        "SSSS CPU compile is unavailable on the Diligent build");
#endif
}

TResult<FSubsurfaceScattering::FCompiledShaders>
FSubsurfaceScattering::BeginCompileShadersAsync(
    IRhiDevice& device) noexcept {
    if (!device.SupportsAsyncShaderCompilation()) {
        return ACS_ERR(
            Render, 360,
            "SSSS backend-managed asynchronous compilation is unsupported");
    }

    FCompiledShaders shaders{};
    FShaderDesc vertex_description{};
    vertex_description.stage = EShaderStage::Vertex;
    vertex_description.hlsl_source = kSubsurfaceScatteringHlsl;
    vertex_description.entry_point = "VSMain";
    vertex_description.debug_name = "SubsurfaceScattering.VS";
    vertex_description.compile_async = true;
    auto vertex = CreateRhiShader(device, vertex_description);
    if (vertex.IsErr()) return Err<FCompiledShaders>(vertex.Error());
    shaders.vertex = Move(vertex.Value());

    FShaderDesc blur_description{};
    blur_description.stage = EShaderStage::Pixel;
    blur_description.hlsl_source = kSubsurfaceScatteringHlsl;
    blur_description.entry_point = "PSBlur";
    blur_description.debug_name = "SubsurfaceScattering.BlurPS";
    blur_description.compile_async = true;
    auto blur = CreateRhiShader(device, blur_description);
    if (blur.IsErr()) return Err<FCompiledShaders>(blur.Error());
    shaders.blur_pixel = Move(blur.Value());

    FShaderDesc composite_description{};
    composite_description.stage = EShaderStage::Pixel;
    composite_description.hlsl_source = kSubsurfaceScatteringHlsl;
    composite_description.entry_point = "PSComposite";
    composite_description.debug_name = "SubsurfaceScattering.CompositePS";
    composite_description.compile_async = true;
    auto composite = CreateRhiShader(device, composite_description);
    if (composite.IsErr()) {
        return Err<FCompiledShaders>(composite.Error());
    }
    shaders.composite_pixel = Move(composite.Value());
    return TResult<FCompiledShaders>(OkInit, Move(shaders));
}

TResult<void> FSubsurfaceScattering::InitWithCompiledShaders(
    IRhiDevice& device,
    FCompiledShaders&& shaders,
    u32 width,
    u32 height) noexcept {
    if (width == 0 || height == 0) {
        return ACS_ERR(
            Render, 350,
            "FSubsurfaceScattering::Init requires non-zero dimensions");
    }
    if (shaders.Status() != EShaderStatus::Ready) {
        return ACS_ERR(
            Render, 361, "SSSS compiled shader set is not ready");
    }

    // Build a complete candidate first so reinitialization failure leaves the
    // currently usable object untouched.
    FSubsurfaceScattering candidate;
    candidate.m_Device = &device;
    if (auto result =
            candidate.CreatePipelines(device, shaders);
        result.IsErr()) {
        return result;
    }
    if (auto result = candidate.CreateConstantBuffers(device); result.IsErr()) {
        return result;
    }
    if (auto result = candidate.CreateTargets(device, width, height);
        result.IsErr()) {
        return result;
    }
    candidate.m_Width = width;
    candidate.m_Height = height;

    Shutdown();
    m_Device = candidate.m_Device;
    m_Width = candidate.m_Width;
    m_Height = candidate.m_Height;
    m_Horizontal = Move(candidate.m_Horizontal);
    m_Output = Move(candidate.m_Output);
    m_VertexShader = Move(candidate.m_VertexShader);
    m_BlurPixelShader = Move(candidate.m_BlurPixelShader);
    m_CompositePixelShader = Move(candidate.m_CompositePixelShader);
    m_BlurPipeline = Move(candidate.m_BlurPipeline);
    m_CompositePipeline = Move(candidate.m_CompositePipeline);
    m_HorizontalCb = Move(candidate.m_HorizontalCb);
    m_VerticalCb = Move(candidate.m_VerticalCb);
    return Ok();
}

TResult<void>
FSubsurfaceScattering::InitPipelineResourcesWithCompiledShaders(
    IRhiDevice& device,
    FCompiledShaders&& shaders) noexcept {
    if (shaders.Status() != EShaderStatus::Ready) {
        return ACS_ERR(
            Render, 363, "SSSS compiled shader set is not ready");
    }

    // Construct every pipeline-side resource before touching the published
    // object. Full-resolution targets are a later Resize commit.
    FSubsurfaceScattering candidate;
    candidate.m_Device = &device;
    if (auto result = candidate.CreatePipelines(device, shaders);
        result.IsErr()) {
        return result;
    }
    if (auto result = candidate.CreateConstantBuffers(device);
        result.IsErr()) {
        return result;
    }

    Shutdown();
    m_Device = candidate.m_Device;
    m_VertexShader = Move(candidate.m_VertexShader);
    m_BlurPixelShader = Move(candidate.m_BlurPixelShader);
    m_CompositePixelShader = Move(candidate.m_CompositePixelShader);
    m_BlurPipeline = Move(candidate.m_BlurPipeline);
    m_CompositePipeline = Move(candidate.m_CompositePipeline);
    m_HorizontalCb = Move(candidate.m_HorizontalCb);
    m_VerticalCb = Move(candidate.m_VerticalCb);
    return Ok();
}

TResult<void> FSubsurfaceScattering::BuildPipelineCandidateForRawDx12(
    IRhiDevice& device,
    FCompiledShaders&& shaders) noexcept {
#if !WITH_RENDER_DILIGENT
    return InitPipelineResourcesWithCompiledShaders(
        device, Move(shaders));
#else
    (void)device;
    (void)shaders;
    return ACS_ERR(
        Render, 364,
        "SSSS raw-DX12 candidate construction is unavailable on "
        "the Diligent build");
#endif
}

void FSubsurfaceScattering::Shutdown() noexcept {
    m_CompositePipeline.Reset();
    m_BlurPipeline.Reset();
    m_VerticalCb.Reset();
    m_HorizontalCb.Reset();
    m_CompositePixelShader.Reset();
    m_BlurPixelShader.Reset();
    m_VertexShader.Reset();
    m_Output.Reset();
    m_Horizontal.Reset();
    m_Device = nullptr;
    m_Width = 0;
    m_Height = 0;
}

TResult<void> FSubsurfaceScattering::Resize(
    u32 width, u32 height) noexcept {
    if (!m_Device) {
        return ACS_ERR(
            Render, 351,
            "FSubsurfaceScattering::Resize called before Init");
    }
    if (width == 0 || height == 0) {
        return ACS_ERR(
            Render, 352,
            "FSubsurfaceScattering::Resize requires non-zero dimensions");
    }
    if (width == m_Width && height == m_Height) return Ok();

    auto result = CreateTargets(*m_Device, width, height);
    if (result.IsErr()) return result;
    m_Width = width;
    m_Height = height;
    return Ok();
}

TResult<void> FSubsurfaceScattering::CreateTargets(
    IRhiDevice& device, u32 width, u32 height) noexcept {
    FTextureDesc description{};
    description.width = width;
    description.height = height;
    description.format = EFormat::R16G16B16A16_Float;
    description.is_render_target = true;

    auto horizontal_result = CreateRhiTexture(device, description);
    if (horizontal_result.IsErr()) {
        return Err<void>(horizontal_result.Error());
    }
    auto output_result = CreateRhiTexture(device, description);
    if (output_result.IsErr()) {
        return Err<void>(output_result.Error());
    }

    // Publish only the complete matching pair.
    m_Horizontal = Move(horizontal_result.Value());
    m_Output = Move(output_result.Value());
    return Ok();
}

TResult<void> FSubsurfaceScattering::CreateConstantBuffers(
    IRhiDevice& device) noexcept {
    FBufferDesc description{};
    description.size = ConstantBufferSize();
    description.usage = EBufferUsage::Uniform;
    description.cpu_writable = true;

    auto horizontal_result = CreateRhiBuffer(device, description);
    if (horizontal_result.IsErr()) {
        return Err<void>(horizontal_result.Error());
    }
    auto vertical_result = CreateRhiBuffer(device, description);
    if (vertical_result.IsErr()) {
        return Err<void>(vertical_result.Error());
    }

    m_HorizontalCb = Move(horizontal_result.Value());
    m_VerticalCb = Move(vertical_result.Value());
    return Ok();
}

TResult<void> FSubsurfaceScattering::CreatePipelines(
    IRhiDevice& device,
    FCompiledShaders& shaders) noexcept {
    if (shaders.Status() != EShaderStatus::Ready) {
        return ACS_ERR(
            Render, 362, "SSSS pipeline shaders are not ready");
    }

    FPipelineDesc blur_pipeline{};
    blur_pipeline.vs = shaders.vertex.Get();
    blur_pipeline.ps = shaders.blur_pixel.Get();
    blur_pipeline.topology = EPrimitiveTopology::TriangleList;
    blur_pipeline.rt_format = EFormat::R16G16B16A16_Float;
    blur_pipeline.depth_format = EFormat::Unknown;
    blur_pipeline.depth_test = false;
    blur_pipeline.depth_write = false;
    blur_pipeline.cull_mode = ECullMode::None;
    blur_pipeline.blend_mode = EBlendMode::Opaque;
    blur_pipeline.cbuffer_slots = 1;
    blur_pipeline.cbuffer_names[0] = "SsssCB";
    blur_pipeline.texture_slots = 4;
    blur_pipeline.texture_names[0] = "diffuse_input";
    blur_pipeline.texture_names[1] = "scene_depth";
    blur_pipeline.texture_names[2] = "normal_gbuffer";
    blur_pipeline.texture_names[3] = "material_data";
    blur_pipeline.static_sampler_count = 4;
    blur_pipeline.static_samplers[0] = LinearClampSampler();
    blur_pipeline.static_samplers[1] = PointClampSampler();
    blur_pipeline.static_samplers[2] = PointClampSampler();
    blur_pipeline.static_samplers[3] = PointClampSampler();
    auto blur_pipeline_result =
        CreateRhiPipeline(device, blur_pipeline);
    if (blur_pipeline_result.IsErr()) {
        return Err<void>(blur_pipeline_result.Error());
    }
    m_BlurPipeline = Move(blur_pipeline_result.Value());

    FPipelineDesc composite_pipeline = blur_pipeline;
    composite_pipeline.ps = shaders.composite_pixel.Get();
    composite_pipeline.texture_slots = 6;
    composite_pipeline.texture_names[4] = "original_diffuse";
    composite_pipeline.texture_names[5] = "scene_color";
    composite_pipeline.static_sampler_count = 6;
    composite_pipeline.static_samplers[4] = LinearClampSampler();
    composite_pipeline.static_samplers[5] = LinearClampSampler();
    auto composite_pipeline_result =
        CreateRhiPipeline(device, composite_pipeline);
    if (composite_pipeline_result.IsErr()) {
        return Err<void>(composite_pipeline_result.Error());
    }
    m_CompositePipeline = Move(composite_pipeline_result.Value());
    m_VertexShader = Move(shaders.vertex);
    m_BlurPixelShader = Move(shaders.blur_pixel);
    m_CompositePixelShader = Move(shaders.composite_pixel);
    return Ok();
}

bool FSubsurfaceScattering::Render(
    IRhiCommandList& command_list,
    IRhiTexture& scene_color,
    IRhiTexture& diffuse_lighting,
    IRhiTexture& scene_depth,
    IRhiTexture& normal_gbuffer,
    IRhiTexture& material_data,
    const FMat4& inverse_view_projection,
    const FSubsurfaceScatteringParams& params) noexcept {
    FSubsurfaceScatteringParams safe_params = params;
    safe_params.Sanitize();
    if (!safe_params.enabled || safe_params.strength <= 1e-4f ||
        !IsReady() || m_Width == 0 || m_Height == 0) {
        return false;
    }

    const auto dimensions_match = [this](const IRhiTexture& texture) {
        return texture.Width() == m_Width && texture.Height() == m_Height;
    };
    if (!dimensions_match(scene_color) ||
        !dimensions_match(diffuse_lighting) ||
        !dimensions_match(scene_depth) ||
        !dimensions_match(normal_gbuffer) ||
        !dimensions_match(material_data)) {
        return false;
    }

    FSsssCbLayout horizontal{};
    horizontal.inverse_view_projection = inverse_view_projection;
    horizontal.pass_data = FVec4{
        1.0f / static_cast<f32>(m_Width),
        1.0f / static_cast<f32>(m_Height),
        1.0f, 0.0f};
    horizontal.diffusion = FVec4{
        safe_params.radius_world,
        safe_params.strength,
        safe_params.depth_sigma,
        safe_params.normal_power};
    horizontal.profile = FVec4{
        safe_params.channel_radius.x,
        safe_params.channel_radius.y,
        safe_params.channel_radius.z,
        safe_params.max_radius_pixels};

    FSsssCbLayout vertical = horizontal;
    vertical.pass_data.z = 0.0f;
    vertical.pass_data.w = 1.0f;
    m_HorizontalCb->Update(&horizontal, sizeof(horizontal));
    m_VerticalCb->Update(&vertical, sizeof(vertical));

    command_list.BeginRenderToTexture(
        *m_Horizontal, FClearColor{0, 0, 0, 1}, nullptr, 1.0f);
    command_list.SetPipeline(*m_BlurPipeline);
    command_list.SetConstantBuffer(0, *m_HorizontalCb);
    command_list.SetTexture(0, diffuse_lighting);
    command_list.SetTexture(1, scene_depth);
    command_list.SetTexture(2, normal_gbuffer);
    command_list.SetTexture(3, material_data);
    command_list.Draw(3, 0);
    command_list.EndRenderToTexture(*m_Horizontal);

    command_list.BeginRenderToTexture(
        *m_Output, FClearColor{0, 0, 0, 1}, nullptr, 1.0f);
    command_list.SetPipeline(*m_CompositePipeline);
    command_list.SetConstantBuffer(0, *m_VerticalCb);
    command_list.SetTexture(0, *m_Horizontal);
    command_list.SetTexture(1, scene_depth);
    command_list.SetTexture(2, normal_gbuffer);
    command_list.SetTexture(3, material_data);
    command_list.SetTexture(4, diffuse_lighting);
    command_list.SetTexture(5, scene_color);
    command_list.Draw(3, 0);
    command_list.EndRenderToTexture(*m_Output);
    return true;
}

} // namespace acs
