// 標準ライティングシェーダ実装
#include "render/StandardShader.h"
#include "asset/MeshAsset.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <cstring>

namespace acs {

namespace {

// HLSL ソース（VS + PS、row-major で Mat4 をそのまま受け取る）
// 最大 4 灯の有向光源 + 4 灯の点光源 + 環境光 + Blinn-Phong スペキュラ + シャドウマップ
const char* kStandardHLSL = R"(
#pragma pack_matrix(row_major)

#define ACS_MAX_DIR_LIGHTS   4
#define ACS_MAX_POINT_LIGHTS 4

cbuffer Frame : register(b0) {
    float4x4 view_proj;
    float4   camera_pos;                              // xyz=eye, w=pad
    float4   ambient;                                 // xyz=ambient, w=dir_count
    float4   point_count_pad;                         // x=point_count
    float4   light_dir  [ACS_MAX_DIR_LIGHTS];         // xyz=direction TO light, w=pad
    float4   light_color[ACS_MAX_DIR_LIGHTS];         // xyz=color, w=pad
    float4   point_pos_range [ACS_MAX_POINT_LIGHTS];  // xyz=world pos, w=range
    float4   point_color     [ACS_MAX_POINT_LIGHTS];  // xyz=color, w=pad
    float4x4 light_view_proj;                         // for shadow map projection
    float4   shadow_params;                           // x=bias, y=enabled (0/1), z=texel_size
};

cbuffer Object : register(b1) {
    float4x4 model;
    float4   base_color;     // xyz=color, w=alpha
    float4   material;       // x=specular_strength, y=shininess, zw=pad
};

Texture2D    albedo : register(t0);
Texture2D    shadow_map : register(t1);
// 命名規約: <texture>_sampler。Diligent の CombinedSamplerSuffix=_sampler と一致
SamplerState albedo_sampler     : register(s0);
SamplerState shadow_map_sampler : register(s1);

struct VSIn {
    float3 pos    : POSITION;
    float3 nrm    : NORMAL;
    float2 uv     : TEXCOORD0;
};

struct VSOut {
    float4 pos      : SV_POSITION;
    float3 world_p  : POSITION;
    float3 world_n  : NORMAL;
    float2 uv       : TEXCOORD0;
};

VSOut VSMain(VSIn v) {
    VSOut o;
    float4 wp = mul(float4(v.pos, 1.0), model);
    o.world_p = wp.xyz;
    o.pos = mul(wp, view_proj);
    o.world_n = mul(float4(v.nrm, 0.0), model).xyz;
    o.uv = v.uv;
    return o;
}

// シャドウ係数: 1=完全に光が当たる、0=影、PCF で中間値
float ComputeShadow(float3 world_p) {
    if (shadow_params.y < 0.5) return 1.0;     // 無効化
    float4 lp = mul(float4(world_p, 1.0), light_view_proj);
    float3 ndc = lp.xyz / lp.w;
    if (ndc.x < -1.0 || ndc.x > 1.0 ||
        ndc.y < -1.0 || ndc.y > 1.0 ||
        ndc.z <  0.0 || ndc.z > 1.0) return 1.0;
    float2 uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
    float my_d = ndc.z;
    float bias = shadow_params.x;
    float ts   = shadow_params.z;
    // 4-tap PCF
    float lit = 0;
    lit += (shadow_map.SampleLevel(shadow_map_sampler, uv + float2(-ts, -ts), 0).r + bias >= my_d) ? 1.0 : 0.0;
    lit += (shadow_map.SampleLevel(shadow_map_sampler, uv + float2( ts, -ts), 0).r + bias >= my_d) ? 1.0 : 0.0;
    lit += (shadow_map.SampleLevel(shadow_map_sampler, uv + float2(-ts,  ts), 0).r + bias >= my_d) ? 1.0 : 0.0;
    lit += (shadow_map.SampleLevel(shadow_map_sampler, uv + float2( ts,  ts), 0).r + bias >= my_d) ? 1.0 : 0.0;
    return lit * 0.25;
}

float4 PSMain(VSOut v) : SV_TARGET {
    float3 N = normalize(v.world_n);
    float3 V = normalize(camera_pos.xyz - v.world_p);

    float3 albedo_rgb = albedo.Sample(albedo_sampler, v.uv).rgb * base_color.xyz;

    // 環境光
    float3 col = ambient.xyz * albedo_rgb;

    // シャドウ係数（最初の dir light のみ適用）
    float shadow = ComputeShadow(v.world_p);

    // 1) 有向光源（Lambert + Blinn-Phong）。i==0 にのみシャドウを乗算
    int dir_count = (int)ambient.w;
    [unroll]
    for (int i = 0; i < ACS_MAX_DIR_LIGHTS; ++i) {
        if (i >= dir_count) break;
        float3 L = normalize(light_dir[i].xyz);
        float  diff = saturate(dot(N, L));
        float3 H = normalize(L + V);
        float  spec = pow(saturate(dot(N, H)), max(material.y, 1.0)) * material.x;
        float  k    = (i == 0) ? shadow : 1.0;
        col += light_color[i].xyz * (albedo_rgb * diff + spec) * k;
    }

    // 2) 点光源（距離による減衰付き）
    int pt_count = (int)point_count_pad.x;
    [unroll]
    for (int j = 0; j < ACS_MAX_POINT_LIGHTS; ++j) {
        if (j >= pt_count) break;
        float3 to_light = point_pos_range[j].xyz - v.world_p;
        float  dist = length(to_light);
        float  rng  = max(point_pos_range[j].w, 0.0001);
        if (dist >= rng) continue;
        float3 L = to_light / dist;
        float  att = 1.0 - dist / rng;
        att = att * att;                                  // smooth falloff
        float  diff = saturate(dot(N, L)) * att;
        float3 H = normalize(L + V);
        float  spec = pow(saturate(dot(N, H)), max(material.y, 1.0)) * material.x * att;
        col += point_color[j].xyz * (albedo_rgb * diff + spec);
    }

    return float4(col, base_color.w);
}
)";

// CB レイアウト（HLSL と一致、float4 アライン）
constexpr u32 kMaxDirLights   = 4;
constexpr u32 kMaxPointLights = 4;
struct FrameCBLayout {
    Mat4 view_proj;
    Vec4 camera_pos;
    Vec4 ambient;                                  // xyz=ambient, w=dir_count
    Vec4 point_count_pad;                          // x=point_count
    Vec4 light_dir  [kMaxDirLights];
    Vec4 light_color[kMaxDirLights];
    Vec4 point_pos_range[kMaxPointLights];
    Vec4 point_color    [kMaxPointLights];
    Mat4 light_view_proj;
    Vec4 shadow_params;                            // x=bias, y=enabled, z=texel_size
};

struct ObjectCBLayout {
    Mat4 model;
    Vec4 base_color;
    Vec4 material;       // x=specular_strength, y=shininess
};

// CB は 256B にアライン推奨（DX12 制約）
template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

Result<void> StandardShader::Init(IRhiDevice& device, Format rt_format, Format depth_format) noexcept {
    // === シェーダコンパイル ===
    ShaderDesc vs_d{};
    vs_d.stage = ShaderStage::Vertex;
    vs_d.hlsl_source = kStandardHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Standard.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    _vs = Move(vs_r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = ShaderStage::Pixel;
    ps_d.hlsl_source = kStandardHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Standard.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    _ps = Move(ps_r.Value());

    // === 定数バッファ ===
    BufferDesc fcb{};
    fcb.size = CBSize<FrameCBLayout>();
    fcb.usage = BufferUsage::Uniform;
    fcb.cpu_writable = true;
    auto fcb_r = CreateRhiBuffer(device, fcb);
    if (fcb_r.IsErr()) return Err<void>(fcb_r.Error());
    _frame_cb = Move(fcb_r.Value());

    BufferDesc ocb{};
    ocb.size = CBSize<ObjectCBLayout>();
    ocb.usage = BufferUsage::Uniform;
    ocb.cpu_writable = true;
    auto ocb_r = CreateRhiBuffer(device, ocb);
    if (ocb_r.IsErr()) return Err<void>(ocb_r.Error());
    _object_cb = Move(ocb_r.Value());

    // === 1×1 白テクスチャ ===
    const u8 white_pixel[4] = { 255, 255, 255, 255 };
    TextureDesc td{};
    td.width = 1; td.height = 1;
    td.format = Format::R8G8B8A8_UNorm;
    td.initial_data = white_pixel;
    td.initial_data_size = 4;
    auto wt_r = CreateRhiTexture(device, td);
    if (wt_r.IsErr()) return Err<void>(wt_r.Error());
    _white = Move(wt_r.Value());

    // === パイプライン ===
    PipelineDesc pd{};
    pd.vs = _vs.Get();
    pd.ps = _ps.Get();
    pd.topology      = PrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = depth_format;
    pd.depth_test    = depth_format != Format::Unknown;
    pd.depth_write   = true;
    pd.cull_mode     = CullMode::Back;
    pd.cbuffer_slots = 2;     // b0=Frame, b1=Object
    pd.texture_slots = 2;     // t0=albedo, t1=shadow_map
    pd.cbuffer_names[0] = "Frame";
    pd.cbuffer_names[1] = "Object";
    pd.texture_names[0] = "albedo";
    pd.texture_names[1] = "shadow_map";
    pd.static_sampler_count = 2;
    pd.static_samplers[0].filter    = SamplerFilter::Linear;
    pd.static_samplers[0].address_u = SamplerAddress::Wrap;
    pd.static_samplers[0].address_v = SamplerAddress::Wrap;
    pd.static_samplers[1].filter    = SamplerFilter::Linear;
    pd.static_samplers[1].address_u = SamplerAddress::Clamp;
    pd.static_samplers[1].address_v = SamplerAddress::Clamp;
    pd.vertex_stride = sizeof(MeshVertex);
    pd.layout[0] = { "POSITION", 0, Format::R32G32B32_Float, 0  };
    pd.layout[1] = { "NORMAL",   0, Format::R32G32B32_Float, 16 }; // Vec3 はアライン 16
    pd.layout[2] = { "TEXCOORD", 0, Format::R32G32_Float,    32 };
    pd.layout_count = 3;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    _pipeline = Move(pl_r.Value());

    return Ok();
}

void StandardShader::Shutdown() noexcept {
    _pipeline.Reset();
    _white.Reset();
    _object_cb.Reset();
    _frame_cb.Reset();
    _ps.Reset();
    _vs.Reset();
}

void StandardShader::SetFrame(const Mat4& vp, Vec3 cam, Vec3 light_dir,
                              Vec3 light_color, Vec3 ambient) noexcept {
    DirLight one;
    one.direction = light_dir;
    one.color     = light_color;
    SetLights(vp, cam, &one, 1, ambient);
}

void StandardShader::SetLights(const Mat4& vp, Vec3 cam,
                               const DirLight* lights, u32 count,
                               Vec3 ambient) noexcept {
    if (count > kMaxDirLights) count = kMaxDirLights;
    _vp = vp;
    _eye = cam;
    _ambient = ambient;
    _dir_count = count;
    for (u32 i = 0; i < count; ++i) _dir_lights[i] = lights[i];
    FlushFrameCB();
}

void StandardShader::SetPointLights(const PointLight* lights, u32 count) noexcept {
    if (count > kMaxPointLights) count = kMaxPointLights;
    _point_count = count;
    for (u32 i = 0; i < count; ++i) _point_lights[i] = lights[i];
    FlushFrameCB();
}

void StandardShader::SetShadowMap(IRhiTexture* tex, const Mat4& light_vp, f32 bias) noexcept {
    _shadow_tex = tex;
    _light_vp   = light_vp;
    _shadow_bias = bias;
    FlushFrameCB();
}

void StandardShader::FlushFrameCB() noexcept {
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
    cb.light_view_proj = _light_vp;
    // shadow_params: x=bias, y=enabled, z=texel_size, w=pad
    // PCF offset 用の texel_size は「1 texel 分」だと 4 tap が同じ texel に
    // 落ちて binary 化する (= 影エッジが階段状)。隣接 texel に届かせるため
    // 2 texel 分 (= 1/1024) にして bilinear sample で滑らかなエッジを得る。
    // 厳密には ShadowMap::Size() から取るべきだが、現状の固定 2048 想定で
    // 近似値で良い。
    cb.shadow_params = Vec4{
        _shadow_bias,
        _shadow_tex ? 1.0f : 0.0f,
        2.0f / 2048.0f,
        0.0f
    };
    _frame_cb->Update(&cb, sizeof(cb));
}

void StandardShader::SetObject(const Mat4& model, Vec3 base_color,
                               f32 specular_strength, f32 shininess) noexcept {
    if (!_object_cb) return;
    ObjectCBLayout cb{};
    cb.model      = model;
    cb.base_color = Vec4{base_color.x, base_color.y, base_color.z, 1.0f};
    cb.material   = Vec4{specular_strength, shininess, 0, 0};
    _object_cb->Update(&cb, sizeof(cb));
}

void StandardShader::DrawMesh(IRhiCommandList& cmd,
                              const GpuMesh& mesh,
                              const Mat4& model,
                              Vec3 base_color,
                              f32  specular_strength,
                              f32  shininess,
                              IRhiTexture* albedo) noexcept {
    if (!_pipeline || !mesh.vertex_buffer || !mesh.index_buffer) return;
    SetObject(model, base_color, specular_strength, shininess);

    cmd.SetPipeline(*_pipeline);
    cmd.SetConstantBuffer(0, *_frame_cb);
    cmd.SetConstantBuffer(1, *_object_cb);
    cmd.SetTexture(0, albedo ? *albedo : *_white);
    cmd.SetTexture(1, *ShadowTextureOrDefault());
    cmd.SetVertexBuffer(*mesh.vertex_buffer, mesh.vertex_stride);
    cmd.SetIndexBuffer(*mesh.index_buffer);
    cmd.DrawIndexed(mesh.index_count);
}

} // namespace acs
