// SPDX-License-Identifier: Apache-2.0
// FSkinnedShader 実装
#include "render/SkinnedShader.h"
#include "asset/SkinnedMesh.h"        // SkinnedVertex を使う
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <cstring>

namespace acs {

namespace {

const char* kSkinnedHLSL = R"(
#pragma pack_matrix(row_major)

#define ACS_MAX_DIR_LIGHTS   4
#define ACS_MAX_POINT_LIGHTS 4
#define ACS_MAX_BONES        64

cbuffer Frame : register(b0) {
    float4x4 view_proj;
    float4   camera_pos;
    float4   ambient;                                 // xyz=ambient, w=dir_count
    float4   point_count_pad;                         // x=point_count
    float4   light_dir  [ACS_MAX_DIR_LIGHTS];
    float4   light_color[ACS_MAX_DIR_LIGHTS];
    float4   point_pos_range [ACS_MAX_POINT_LIGHTS];
    float4   point_color     [ACS_MAX_POINT_LIGHTS];
};

cbuffer Object : register(b1) {
    float4x4 model;
    float4   base_color;
    float4   material;       // x=specular, y=shininess
};

cbuffer Bones : register(b2) {
    float4x4 bone_palette[ACS_MAX_BONES];
};

Texture2D    albedo : register(t0);
// 命名規約: <texture>m_Sampler
SamplerState albedo_sampler : register(s0);

struct VSIn {
    float3 pos       : POSITION;
    float3 nrm       : NORMAL;
    float2 uv        : TEXCOORD0;
    uint4  bones     : BLENDINDICES;
    float4 weights   : BLENDWEIGHT;
};

struct VSOut {
    float4 pos      : SV_POSITION;
    float3 world_p  : POSITION;
    float3 world_n  : NORMAL;
    float2 uv       : TEXCOORD0;
};

VSOut VSMain(VSIn v) {
    // 4 つのボーンを weights で加重平均してスキニング行列を作る
    float4x4 skin = bone_palette[v.bones.x] * v.weights.x +
                    bone_palette[v.bones.y] * v.weights.y +
                    bone_palette[v.bones.z] * v.weights.z +
                    bone_palette[v.bones.w] * v.weights.w;

    // バインドポーズ位置 → アニメーション位置（オブジェクト空間内）
    float4 animated_p = mul(float4(v.pos, 1.0), skin);
    float4 animated_n = mul(float4(v.nrm, 0.0), skin);

    // モデル行列を適用してワールド空間へ
    float4 wp = mul(animated_p, model);

    VSOut o;
    o.world_p = wp.xyz;
    o.pos     = mul(wp, view_proj);
    o.world_n = mul(animated_n, model).xyz;
    o.uv      = v.uv;
    return o;
}

float4 PSMain(VSOut v) : SV_TARGET {
    float3 N = normalize(v.world_n);
    float3 V = normalize(camera_pos.xyz - v.world_p);
    float3 albedo_rgb = albedo.Sample(albedo_sampler, v.uv).rgb * base_color.xyz;
    float3 col = ambient.xyz * albedo_rgb;

    int dir_count = (int)ambient.w;
    [unroll]
    for (int i = 0; i < ACS_MAX_DIR_LIGHTS; ++i) {
        if (i >= dir_count) break;
        float3 L = normalize(light_dir[i].xyz);
        float  diff = saturate(dot(N, L));
        float3 H    = normalize(L + V);
        float  spec = pow(saturate(dot(N, H)), max(material.y, 1.0)) * material.x;
        col += light_color[i].xyz * (albedo_rgb * diff + spec);
    }

    int pt_count = (int)point_count_pad.x;
    [unroll]
    for (int j = 0; j < ACS_MAX_POINT_LIGHTS; ++j) {
        if (j >= pt_count) break;
        float3 to_light = point_pos_range[j].xyz - v.world_p;
        float  dist = length(to_light);
        float  rng  = max(point_pos_range[j].w, 0.0001);
        if (dist >= rng) continue;
        float3 L = to_light / dist;
        float  att = 1.0 - dist / rng; att = att * att;
        float  diff = saturate(dot(N, L)) * att;
        float3 H = normalize(L + V);
        float  spec = pow(saturate(dot(N, H)), max(material.y, 1.0)) * material.x * att;
        col += point_color[j].xyz * (albedo_rgb * diff + spec);
    }

    return float4(col, base_color.w);
}
)";

constexpr u32 kMaxDirLights   = 4;
constexpr u32 kMaxPointLights = 4;
struct FrameCBLayout {
    FMat4 view_proj;
    FVec4 camera_pos;
    FVec4 ambient;
    FVec4 point_count_pad;
    FVec4 light_dir[kMaxDirLights];
    FVec4 light_color[kMaxDirLights];
    FVec4 point_pos_range[kMaxPointLights];
    FVec4 point_color[kMaxPointLights];
};

struct ObjectCBLayout {
    FMat4 model;
    FVec4 base_color;
    FVec4 material;
};

struct BonesCBLayout {
    FMat4 palette[FSkinnedShader::kMaxBones];
};

template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

TResult<void> FSkinnedShader::Init(IRhiDevice& device, EFormat rt_format, EFormat depth_format) noexcept {
    // === シェーダ ===
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kSkinnedHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Skinned.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    m_Vs = Move(vs_r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kSkinnedHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Skinned.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    m_Ps = Move(ps_r.Value());

    // === 定数バッファ ===
    FBufferDesc fcb{};
    fcb.size = CBSize<FrameCBLayout>();
    fcb.usage = EBufferUsage::Uniform;
    fcb.cpu_writable = true;
    auto fcb_r = CreateRhiBuffer(device, fcb);
    if (fcb_r.IsErr()) return Err<void>(fcb_r.Error());
    m_FrameCb = Move(fcb_r.Value());

    FBufferDesc ocb{};
    ocb.size = CBSize<ObjectCBLayout>();
    ocb.usage = EBufferUsage::Uniform;
    ocb.cpu_writable = true;
    auto ocb_r = CreateRhiBuffer(device, ocb);
    if (ocb_r.IsErr()) return Err<void>(ocb_r.Error());
    m_ObjectCb = Move(ocb_r.Value());

    FBufferDesc bcb{};
    bcb.size = CBSize<BonesCBLayout>();        // 64 * 64 = 4096B、256 アラインで 4096
    bcb.usage = EBufferUsage::Uniform;
    bcb.cpu_writable = true;
    auto bcb_r = CreateRhiBuffer(device, bcb);
    if (bcb_r.IsErr()) return Err<void>(bcb_r.Error());
    m_BonesCb = Move(bcb_r.Value());

    // ボーンを最初は単位行列で初期化しておく（SetBonePalette 前に Draw されてもおかしくならない）
    BonesCBLayout initial{};
    for (u32 i = 0; i < kMaxBones; ++i) initial.palette[i] = FMat4::Identity();
    m_BonesCb->Update(&initial, sizeof(initial));

    // === 1×1 白テクスチャ ===
    const u8 white_pixel[4] = { 255, 255, 255, 255 };
    FTextureDesc td{};
    td.width = 1; td.height = 1;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = white_pixel;
    td.initial_data_size = 4;
    auto wt_r = CreateRhiTexture(device, td);
    if (wt_r.IsErr()) return Err<void>(wt_r.Error());
    m_White = Move(wt_r.Value());

    // === パイプライン ===
    FPipelineDesc pd{};
    pd.vs = m_Vs.Get();
    pd.ps = m_Ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = depth_format;
    pd.depth_test    = depth_format != EFormat::Unknown;
    pd.depth_write   = true;
    pd.cull_mode     = ECullMode::Back;
    pd.cbuffer_slots = 3;     // b0=Frame, b1=Object, b2=Bones
    pd.texture_slots = 1;
    pd.cbuffer_names[0] = "Frame";
    pd.cbuffer_names[1] = "Object";
    pd.cbuffer_names[2] = "Bones";
    pd.texture_names[0] = "albedo";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Wrap;
    pd.static_samplers[0].address_v = ESamplerAddress::Wrap;
    pd.vertex_stride = sizeof(FSkinnedVertex);    // 64
    pd.layout[0] = { "POSITION",     0, EFormat::R32G32B32_Float,    0  };
    pd.layout[1] = { "NORMAL",       0, EFormat::R32G32B32_Float,    16 };
    pd.layout[2] = { "TEXCOORD",     0, EFormat::R32G32_Float,       32 };
    pd.layout[3] = { "BLENDINDICES", 0, EFormat::R8G8B8A8_UInt,      40 };
    pd.layout[4] = { "BLENDWEIGHT",  0, EFormat::R32G32B32A32_Float, 44 };
    pd.layout_count = 5;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    m_Pipeline = Move(pl_r.Value());

    return Ok();
}

void FSkinnedShader::Shutdown() noexcept {
    m_Pipeline.Reset();
    m_White.Reset();
    m_BonesCb.Reset();
    m_ObjectCb.Reset();
    m_FrameCb.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
}

void FSkinnedShader::SetFrame(const FMat4& vp, FVec3 cam, FVec3 light_dir,
                             FVec3 light_color, FVec3 ambient) noexcept {
    FDirLight one;
    one.direction = light_dir;
    one.color     = light_color;
    SetLights(vp, cam, &one, 1, ambient);
}

void FSkinnedShader::SetLights(const FMat4& vp, FVec3 cam,
                              const FDirLight* lights, u32 count,
                              FVec3 ambient) noexcept {
    if (count > kMaxDirLights) count = kMaxDirLights;
    m_Vp = vp;
    m_Eye = cam;
    m_Ambient = ambient;
    m_DirCount = count;
    for (u32 i = 0; i < count; ++i) m_DirLights[i] = lights[i];
    FlushFrameCB();
}

void FSkinnedShader::SetPointLights(const PointLight* lights, u32 count) noexcept {
    if (count > kMaxPointLights) count = kMaxPointLights;
    m_PointCount = count;
    for (u32 i = 0; i < count; ++i) m_PointLights[i] = lights[i];
    FlushFrameCB();
}

void FSkinnedShader::FlushFrameCB() noexcept {
    if (!m_FrameCb) return;
    FrameCBLayout cb{};
    cb.view_proj  = m_Vp;
    cb.camera_pos = FVec4{m_Eye.x, m_Eye.y, m_Eye.z, 1.0f};
    cb.ambient    = FVec4{m_Ambient.x, m_Ambient.y, m_Ambient.z, static_cast<f32>(m_DirCount)};
    cb.point_count_pad = FVec4{static_cast<f32>(m_PointCount), 0, 0, 0};
    for (u32 i = 0; i < m_DirCount; ++i) {
        const FVec3& d = m_DirLights[i].direction;
        const FVec3& c = m_DirLights[i].color;
        cb.light_dir[i]   = FVec4{d.x, d.y, d.z, 0};
        cb.light_color[i] = FVec4{c.x, c.y, c.z, 1};
    }
    for (u32 i = 0; i < m_PointCount; ++i) {
        const FVec3& p = m_PointLights[i].position;
        const FVec3& c = m_PointLights[i].color;
        cb.point_pos_range[i] = FVec4{p.x, p.y, p.z, m_PointLights[i].range};
        cb.point_color[i]     = FVec4{c.x, c.y, c.z, 1};
    }
    m_FrameCb->Update(&cb, sizeof(cb));
}

void FSkinnedShader::SetObject(const FMat4& model, FVec3 base_color,
                              f32 specular_strength, f32 shininess) noexcept {
    if (!m_ObjectCb) return;
    ObjectCBLayout cb{};
    cb.model      = model;
    cb.base_color = FVec4{base_color.x, base_color.y, base_color.z, 1.0f};
    cb.material   = FVec4{specular_strength, shininess, 0, 0};
    m_ObjectCb->Update(&cb, sizeof(cb));
}

void FSkinnedShader::SetBonePalette(const FMat4* palette, u32 count) noexcept {
    if (!m_BonesCb) return;
    if (count > kMaxBones) count = kMaxBones;
    BonesCBLayout cb{};
    for (u32 i = 0; i < count; ++i) cb.palette[i] = palette[i];
    for (u32 i = count; i < kMaxBones; ++i) cb.palette[i] = FMat4::Identity();
    m_BonesCb->Update(&cb, sizeof(cb));
}

} // namespace acs
