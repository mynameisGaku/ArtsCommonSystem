// PbrShader 実装 — Cook-Torrance BRDF (GGX + Smith + Schlick)
#include "render/PbrShader.h"
#include "asset/MeshAsset.h"   // MeshVertex
#include "foundation/Move.h"   // Move
#include "foundation/Log.h"

#include <cstring>

namespace acs {

namespace {

#define ACS_MAX_DIR_LIGHTS   4
#define ACS_MAX_POINT_LIGHTS 4

// HLSL: PBR Cook-Torrance。StandardShader と同じ vertex 入力 (pos / nrm / uv)
// + 同じ vs 出力 (world_p / world_n / uv)。
const char* kPbrHLSL = R"(
#pragma pack_matrix(row_major)

#define ACS_MAX_DIR_LIGHTS   4
#define ACS_MAX_POINT_LIGHTS 4

cbuffer Frame : register(b0) {
    float4x4 view_proj;
    float4   camera_pos;                              // xyz=eye, w=pad
    float4   ambient;                                 // xyz=ambient color, w=dir_count
    float4   point_count_pad;                         // x=point_count
    float4   light_dir  [ACS_MAX_DIR_LIGHTS];
    float4   light_color[ACS_MAX_DIR_LIGHTS];
    float4   point_pos_range [ACS_MAX_POINT_LIGHTS];
    float4   point_color     [ACS_MAX_POINT_LIGHTS];
};

cbuffer Object : register(b1) {
    float4x4 model;
    float4   base_color;     // xyz=color, w=alpha
    float4   pbr_params;     // x=metallic, y=roughness, z=ao, w=pad
};

Texture2D    albedo : register(t0);
SamplerState albedo_sampler : register(s0);

struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut {
    float4 pos     : SV_POSITION;
    float3 world_p : POSITION;
    float3 world_n : NORMAL;
    float2 uv      : TEXCOORD0;
};

VSOut VSMain(VSIn v) {
    VSOut o;
    float4 wp = mul(float4(v.pos, 1.0), model);
    o.world_p = wp.xyz;
    o.pos     = mul(wp, view_proj);
    o.world_n = mul(float4(v.nrm, 0.0), model).xyz;
    o.uv      = v.uv;
    return o;
}

static const float PI = 3.14159265358979323846;

// GGX (Trowbridge-Reitz) normal distribution
float D_GGX(float NdotH, float a2) {
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

// Smith joint geometry (Schlick-GGX). k = (roughness+1)^2 / 8 for direct light.
float G_Smith(float NdotV, float NdotL, float roughness) {
    float k = (roughness + 1.0);
    k = k * k * 0.125;
    float Gv = NdotV / max(NdotV * (1.0 - k) + k, 1e-7);
    float Gl = NdotL / max(NdotL * (1.0 - k) + k, 1e-7);
    return Gv * Gl;
}

// Schlick Fresnel approximation
float3 F_Schlick(float VdotH, float3 F0) {
    float f = pow(saturate(1.0 - VdotH), 5.0);
    return F0 + (1.0 - F0) * f;
}

// Cook-Torrance BRDF * NdotL (light vector L is from surface to light source).
float3 BrdfCookTorrance(float3 N, float3 V, float3 L,
                       float3 base, float metallic, float roughness)
{
    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0) return float3(0, 0, 0);
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    // 非金属の標準 F0 = 0.04 (sRGB linear)、金属は base_color を tint に。
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), base, metallic);
    float  a  = roughness * roughness;
    float  a2 = a * a;

    float  D = D_GGX(NdotH, a2);
    float  G = G_Smith(NdotV, NdotL, roughness);
    float3 F = F_Schlick(VdotH, F0);

    float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-7);
    float3 kd = (1.0 - F) * (1.0 - metallic);        // 金属は diffuse 無し
    float3 diffuse = kd * base / PI;
    return (diffuse + specular) * NdotL;
}

float4 PSMain(VSOut v) : SV_TARGET {
    float3 N = normalize(v.world_n);
    float3 V = normalize(camera_pos.xyz - v.world_p);

    float3 albedo_rgb = albedo.Sample(albedo_sampler, v.uv).rgb * base_color.xyz;
    float  metallic   = pbr_params.x;
    float  roughness  = max(pbr_params.y, 0.04);     // 0 は数値不安定
    float  ao         = pbr_params.z;

    // 環境光 (IBL 未実装、Phase 31 で置換予定)。AO は ambient のみに乗せる。
    float3 col = ambient.xyz * albedo_rgb * ao;

    // 有向光源
    int dir_count = (int)ambient.w;
    [unroll]
    for (int i = 0; i < ACS_MAX_DIR_LIGHTS; ++i) {
        if (i >= dir_count) break;
        float3 L = normalize(light_dir[i].xyz);
        col += light_color[i].xyz * BrdfCookTorrance(N, V, L, albedo_rgb, metallic, roughness);
    }

    // 点光源 (距離減衰)
    int pt_count = (int)point_count_pad.x;
    [unroll]
    for (int j = 0; j < ACS_MAX_POINT_LIGHTS; ++j) {
        if (j >= pt_count) break;
        float3 to_light = point_pos_range[j].xyz - v.world_p;
        float  dist = length(to_light);
        float  rng  = max(point_pos_range[j].w, 1e-4);
        if (dist >= rng) continue;
        float3 L = to_light / max(dist, 1e-4);
        float  att = 1.0 - dist / rng;
        att = att * att;
        col += point_color[j].xyz * BrdfCookTorrance(N, V, L, albedo_rgb, metallic, roughness) * att;
    }

    return float4(col, base_color.w);
}
)";

constexpr u32 kMaxDirLights   = 4;
constexpr u32 kMaxPointLights = 4;
struct FrameCBLayout {
    Mat4 view_proj;
    Vec4 camera_pos;
    Vec4 ambient;
    Vec4 point_count_pad;
    Vec4 light_dir   [kMaxDirLights];
    Vec4 light_color [kMaxDirLights];
    Vec4 point_pos_range[kMaxPointLights];
    Vec4 point_color    [kMaxPointLights];
};

struct ObjectCBLayout {
    Mat4 model;
    Vec4 base_color;
    Vec4 pbr_params;        // x=metallic, y=roughness, z=ao, w=pad
};

template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

Result<void> PbrShader::Init(IRhiDevice& device, Format rt_format, Format depth_format) noexcept {
    ShaderDesc vs_d{};
    vs_d.stage = ShaderStage::Vertex;
    vs_d.hlsl_source = kPbrHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Pbr.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr())
        return Err<void>(r.Error());
    else _vs = Move(r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = ShaderStage::Pixel;
    ps_d.hlsl_source = kPbrHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Pbr.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr())
        return Err<void>(r.Error());
    else _ps = Move(r.Value());

    BufferDesc fb{};
    fb.size = CBSize<FrameCBLayout>();
    fb.usage = BufferUsage::Uniform;
    fb.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, fb); r.IsErr())
        return Err<void>(r.Error());
    else _frame_cb = Move(r.Value());

    BufferDesc ob{};
    ob.size = CBSize<ObjectCBLayout>();
    ob.usage = BufferUsage::Uniform;
    ob.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, ob); r.IsErr())
        return Err<void>(r.Error());
    else _object_cb = Move(r.Value());

    // 1x1 白テクスチャ
    const u8 white[4] = {255, 255, 255, 255};
    TextureDesc td{};
    td.width = 1; td.height = 1;
    td.format = Format::R8G8B8A8_UNorm;
    td.initial_data = white; td.initial_data_size = 4;
    if (auto r = CreateRhiTexture(device, td); r.IsErr())
        return Err<void>(r.Error());
    else _white = Move(r.Value());

    PipelineDesc pd{};
    pd.vs = _vs.Get();
    pd.ps = _ps.Get();
    pd.topology      = PrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = depth_format;
    pd.depth_test    = true;
    pd.depth_write   = true;
    pd.cull_mode     = CullMode::Back;
    pd.cbuffer_slots = 2;     // b0=Frame, b1=Object
    pd.texture_slots = 1;
    pd.cbuffer_names[0] = "Frame";
    pd.cbuffer_names[1] = "Object";
    pd.texture_names[0] = "albedo";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = SamplerFilter::Linear;
    pd.static_samplers[0].address_u = SamplerAddress::Wrap;
    pd.static_samplers[0].address_v = SamplerAddress::Wrap;
    pd.vertex_stride = sizeof(MeshVertex);
    // MeshVertex の Vec3 は alignas(16) で 16 バイト境界。
    // → position@0, normal@16, uv@32 (Standard と一致)。
    // 12/24 にしてしまうと normal が position パディング + normal の途中を読んで
    // ジオメトリが破壊され、PBR が「黒っぽくべったり影」状態に見える。
    pd.layout[0] = { "POSITION", 0, Format::R32G32B32_Float, 0  };
    pd.layout[1] = { "NORMAL",   0, Format::R32G32B32_Float, 16 };
    pd.layout[2] = { "TEXCOORD", 0, Format::R32G32_Float,    32 };
    pd.layout_count = 3;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr())
        return Err<void>(r.Error());
    else _pipeline = Move(r.Value());

    return Ok();
}

void PbrShader::Shutdown() noexcept {
    _pipeline.Reset();
    _object_cb.Reset();
    _frame_cb.Reset();
    _white.Reset();
    _ps.Reset();
    _vs.Reset();
}

void PbrShader::SetLights(const Mat4& vp, Vec3 eye,
                          const DirLight* lights, u32 count,
                          Vec3 ambient) noexcept {
    _vp = vp;
    _eye = eye;
    _ambient = ambient;
    if (count > kMaxDirLights) count = kMaxDirLights;
    _dir_count = count;
    for (u32 i = 0; i < count; ++i) _dir_lights[i] = lights[i];
    FlushFrameCB();
}

void PbrShader::SetPointLights(const PointLight* lights, u32 count) noexcept {
    if (count > kMaxPointLights) count = kMaxPointLights;
    _point_count = count;
    for (u32 i = 0; i < count; ++i) _point_lights[i] = lights[i];
    FlushFrameCB();
}

void PbrShader::FlushFrameCB() noexcept {
    if (!_frame_cb) return;
    FrameCBLayout cb{};
    cb.view_proj  = _vp;
    cb.camera_pos = Vec4{_eye.x, _eye.y, _eye.z, 1.0f};
    cb.ambient    = Vec4{_ambient.x, _ambient.y, _ambient.z, static_cast<f32>(_dir_count)};
    cb.point_count_pad = Vec4{static_cast<f32>(_point_count), 0, 0, 0};
    for (u32 i = 0; i < _dir_count; ++i) {
        const Vec3& d = _dir_lights[i].direction;
        const Vec3& c = _dir_lights[i].color;
        cb.light_dir[i]   = Vec4{d.x, d.y, d.z, 0};
        cb.light_color[i] = Vec4{c.x, c.y, c.z, 1};
    }
    for (u32 i = 0; i < _point_count; ++i) {
        const Vec3& p = _point_lights[i].position;
        const Vec3& c = _point_lights[i].color;
        cb.point_pos_range[i] = Vec4{p.x, p.y, p.z, _point_lights[i].range};
        cb.point_color[i]     = Vec4{c.x, c.y, c.z, 1};
    }
    _frame_cb->Update(&cb, sizeof(cb));
}

void PbrShader::SetObject(const Mat4& model, Vec3 base_color,
                          f32 metallic, f32 roughness, f32 ao) noexcept {
    if (!_object_cb) return;
    ObjectCBLayout cb{};
    cb.model = model;
    cb.base_color = Vec4{base_color.x, base_color.y, base_color.z, 1.0f};
    cb.pbr_params = Vec4{metallic, roughness, ao, 0};
    _object_cb->Update(&cb, sizeof(cb));
}

void PbrShader::DrawMesh(IRhiCommandList& cmd, const GpuMesh& mesh, const Mat4& model,
                        Vec3 base_color, f32 metallic, f32 roughness, f32 ao,
                        IRhiTexture* albedo) noexcept {
    if (!_pipeline || !_frame_cb || !_object_cb) return;
    SetObject(model, base_color, metallic, roughness, ao);
    cmd.SetPipeline(*_pipeline);
    cmd.SetConstantBuffer(0, *_frame_cb);
    cmd.SetConstantBuffer(1, *_object_cb);
    cmd.SetTexture(0, *(albedo ? albedo : _white.Get()));
    cmd.SetVertexBuffer(*mesh.vertex_buffer, mesh.vertex_stride);
    cmd.SetIndexBuffer(*mesh.index_buffer);
    cmd.DrawIndexed(mesh.index_count);
}

} // namespace acs
