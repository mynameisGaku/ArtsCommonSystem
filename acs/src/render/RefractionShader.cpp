// SPDX-License-Identifier: Apache-2.0
// スクリーンスペース屈折シェーダ実装 (Phase 3)
#include "render/RefractionShader.h"
#include "asset/MeshAsset.h"
#include "foundation/Move.h"

namespace acs {

namespace {

// HLSL ソース (VS + PS、row-major)。
// 屈折: 視線を refract() で曲げ、その先の点を screen へ投影して background を sample。
// 反射: Schlick Fresnel で環境キューブマップと blend (grazing 角ほど反射が強い)。
const char* kRefractionHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer Frame : register(b0) {
    float4x4 view_proj;
    float4   camera_pos;     // xyz=eye
    // Phase 35-3f thickness map
    float4   back_params;    // x=enabled (0/1), y=near, z=far, w=pad
    float4   screen_params;  // x=1/screen_w, y=1/screen_h, zw=pad
};

cbuffer Object : register(b1) {
    float4x4 model;
    float4   material;       // x=ior, y=thickness, z=roughness, w=dispersion (Phase 35-3e)
    float4   tint;           // xyz=glass tint (吸収色), w=pad
};

Texture2D    background : register(t0);   // opaque シーンの複製
TextureCube  env        : register(t1);   // Fresnel 反射用の環境マップ
Texture2D    back_depth : register(t2);   // Phase 35-3f: 透明物体の背面深度 (D32_Float SRV)
SamplerState background_sampler : register(s0);
SamplerState env_sampler        : register(s1);
SamplerState back_depth_sampler : register(s2);

struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float3 world_p : POSITION; float3 world_n : NORMAL; };

VSOut VSMain(VSIn v) {
    VSOut o;
    float4 wp = mul(float4(v.pos, 1.0), model);
    o.world_p = wp.xyz;
    o.pos     = mul(wp, view_proj);
    o.world_n = mul(float4(v.nrm, 0.0), model).xyz;
    return o;
}

// 屈折 UV を 1 つの IOR について計算するヘルパー。
//   N        : 法線 (world)、V: surface->eye (world、正規化)、eta: n1/n2
//   world_p  : 表面 world 位置 (exitPoint 計算の起点)
//   thickness: 屈折先までの world 距離 (material.y)
// 戻り値: screen UV ([0,1] にクランプ前)。clip.w < ~0 の場合は world_p の投影を返す。
float2 ComputeRefractUV(float3 N, float3 V, float eta, float3 world_p, float thickness) {
    float3 refractDir = refract(-V, N, eta);
    if (dot(refractDir, refractDir) < 1e-4) refractDir = -V;   // TIR 保険
    float3 exitPoint = world_p + refractDir * thickness;
    float4 clip = mul(float4(exitPoint, 1.0), view_proj);
    if (clip.w < 1e-4) clip = mul(float4(world_p, 1.0), view_proj);
    return clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
}

float4 PSMain(VSOut v) : SV_TARGET {
    float3 N = normalize(v.world_n);
    float3 V = normalize(camera_pos.xyz - v.world_p);   // surface->eye

    // --- IOR / dispersion (Phase 35-3e) ---
    // chromatic dispersion: 値 > 0 のとき R/G/B で IOR をずらし、プリズム/
    // ダイヤ風に色を分離。物理的には Cauchy 分散の単純化 (R が IOR 低、B が高)。
    float ior        = max(material.x, 1.0);
    float dispersion = saturate(material.w);
    float ior_r = max(ior - dispersion * 0.04, 1.0);   // 赤は屈折弱め
    float ior_g = ior;
    float ior_b = ior + dispersion * 0.04;             // 青は屈折強め
    float eta_r = 1.0 / ior_r;
    float eta_g = 1.0 / ior_g;
    float eta_b = 1.0 / ior_b;

    // --- Phase 35-3f thickness map ---
    // 背面 (cull=Front で焼いた) 深度を sample し、表/背の view-space z 差から
    // 実厚みを得る。スカラー fallback は material.y (=既存挙動)。
    //   z_view = n*f / (f - d*(f-n))   (LH perspective, NDC z ∈ [0,1])
    float thickness_used = material.y;
    if (back_params.x >= 0.5) {
        float2 screen_uv = v.pos.xy * screen_params.xy;
        float back_d  = back_depth.SampleLevel(back_depth_sampler, screen_uv, 0).r;
        float front_d = v.pos.z;
        // back_d >= 0.9999 は背面パスが何も書かなかった (=シルエット端等) ので fallback
        if (back_d < 0.9999) {
            float near_z = back_params.y;
            float far_z  = back_params.z;
            float range  = far_z - near_z;
            float back_view  = near_z * far_z / max(far_z - back_d  * range, 1e-4);
            float front_view = near_z * far_z / max(far_z - front_d * range, 1e-4);
            float t_world = back_view - front_view;
            // back が front より手前 (silhouette / 法線反転) なら fallback。
            if (t_world > 0.0) thickness_used = t_world;
        }
    }

    // 屈折先の UV を chunk ごとに計算 (refractUV.G は roughness ブラー / Fresnel
    // 反射ベクトルの基準にもなるので必ず計算)。
    float2 refractUV_g = ComputeRefractUV(N, V, eta_g, v.world_p, thickness_used);
    float2 refractUV_r = (dispersion > 0.001)
                            ? ComputeRefractUV(N, V, eta_r, v.world_p, thickness_used)
                            : refractUV_g;
    float2 refractUV_b = (dispersion > 0.001)
                            ? ComputeRefractUV(N, V, eta_b, v.world_p, thickness_used)
                            : refractUV_g;

    // --- background sample (Phase 35-3d roughness blur + 35-3e dispersion) ---
    // roughness < 0.005 で 1-tap shape paths、それ以外で 8-tap golden disk。
    // dispersion > 0 で RGB を別々に sample して色分離 (clear path のみ 3 sample、
    // frosted path は 3 ch × 8 tap = 24 tap)。
    float  roughness  = saturate(material.z);
    float3 refracted;
    if (roughness < 0.005) {
        // Clear glass — 1 tap per channel
        float r = background.SampleLevel(background_sampler, saturate(refractUV_r), 0).r;
        float g = background.SampleLevel(background_sampler, saturate(refractUV_g), 0).g;
        float b = background.SampleLevel(background_sampler, saturate(refractUV_b), 0).b;
        refracted = float3(r, g, b);
    } else {
        // Frosted glass — 8-tap Vogel disk per channel (uniform branch)
        const float kGoldenAngle = 2.39996323;
        const int   kTaps        = 8;
        const float kRadius      = roughness * 0.02;
        float3 sum = 0;
        [unroll]
        for (int t = 0; t < kTaps; ++t) {
            float ft = (float(t) + 0.5) / float(kTaps);
            float r2 = sqrt(ft) * kRadius;
            float a  = float(t) * kGoldenAngle;
            float2 off = float2(cos(a), sin(a)) * r2;
            float rv = background.SampleLevel(background_sampler, saturate(refractUV_r + off), 0).r;
            float gv = background.SampleLevel(background_sampler, saturate(refractUV_g + off), 0).g;
            float bv = background.SampleLevel(background_sampler, saturate(refractUV_b + off), 0).b;
            sum += float3(rv, gv, bv);
        }
        refracted = sum / float(kTaps);
    }
    refracted *= tint.xyz;                               // ガラスの吸収色

    // --- Fresnel 反射 (環境マップ) ---
    float  NoV = saturate(dot(N, V));
    float  F   = 0.04 + 0.96 * pow(saturate(1.0 - NoV), 5.0);   // F0=0.04 (誘電体)
    float3 reflected = env.SampleLevel(env_sampler, reflect(-V, N), 0).rgb;

    // 屈折と反射を Fresnel で混合
    return float4(lerp(refracted, reflected, F), 1.0);
}
)";

// CB レイアウト (HLSL と一致、float4 アライン)
struct FrameCBLayout {
    FMat4 view_proj;
    FVec4 camera_pos;
    FVec4 back_params;     // Phase 35-3f: x=enabled, y=near, z=far, w=pad
    FVec4 screen_params;   // Phase 35-3f: x=1/w, y=1/h, zw=pad
};

struct ObjectCBLayout {
    FMat4 model;
    FVec4 material;       // x=ior, y=thickness, z=roughness (35-3d), w=dispersion (35-3e)
    FVec4 tint;           // xyz=glass tint
};

// CB は 256B にアライン (DX12 制約)
template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

TResult<void> RefractionShader::Init(IRhiDevice& device, EFormat rt_format,
                                    EFormat depth_format) noexcept {
    // === シェーダコンパイル ===
    ShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kRefractionHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Refraction.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    _vs = Move(vs_r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kRefractionHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Refraction.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    _ps = Move(ps_r.Value());

    // === 定数バッファ ===
    BufferDesc fcb{};
    fcb.size = CBSize<FrameCBLayout>();
    fcb.usage = EBufferUsage::Uniform;
    fcb.cpu_writable = true;
    auto fcb_r = CreateRhiBuffer(device, fcb);
    if (fcb_r.IsErr()) return Err<void>(fcb_r.Error());
    _frame_cb = Move(fcb_r.Value());

    BufferDesc ocb{};
    ocb.size = CBSize<ObjectCBLayout>();
    ocb.usage = EBufferUsage::Uniform;
    ocb.cpu_writable = true;
    auto ocb_r = CreateRhiBuffer(device, ocb);
    if (ocb_r.IsErr()) return Err<void>(ocb_r.Error());
    _object_cb = Move(ocb_r.Value());

    // === パイプライン ===
    PipelineDesc pd{};
    pd.vs = _vs.Get();
    pd.ps = _ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = depth_format;
    pd.depth_test    = depth_format != EFormat::Unknown;
    pd.depth_write   = true;
    pd.cull_mode     = ECullMode::Back;
    pd.blend_mode    = EBlendMode::Opaque;   // SS 屈折は背景を曲げて不透明描画する
    pd.cbuffer_slots = 2;     // b0=Frame, b1=Object
    pd.texture_slots = 3;     // t0=background, t1=env, t2=back_depth (Phase 35-3f)
    pd.cbuffer_names[0] = "Frame";
    pd.cbuffer_names[1] = "Object";
    pd.texture_names[0] = "background";
    pd.texture_names[1] = "env";
    pd.texture_names[2] = "back_depth";
    pd.static_sampler_count = 3;
    pd.static_samplers[0].filter    = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[1].filter    = ESamplerFilter::Linear;
    pd.static_samplers[1].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[1].address_v = ESamplerAddress::Clamp;
    // back_depth は Point sample (深度の bilinear 補間は silhouette でアーティファクト)
    pd.static_samplers[2].filter    = ESamplerFilter::Point;
    pd.static_samplers[2].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[2].address_v = ESamplerAddress::Clamp;
    pd.vertex_stride = sizeof(MeshVertex);
    pd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0  };
    pd.layout[1] = { "NORMAL",   0, EFormat::R32G32B32_Float, 16 }; // FVec3 はアライン 16
    pd.layout[2] = { "TEXCOORD", 0, EFormat::R32G32_Float,    32 };
    pd.layout_count = 3;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    _pipeline = Move(pl_r.Value());

    // Phase 35-3f back_depth fallback: 1x1 R32G32_Float texture。.r = 1.0 を
    // 入れ、shader の「back_d >= 0.9999 → スカラー fallback」分岐に必ず hit
    // させる (R32_Float が EFormat 未定義のため 2ch を採用、.g は捨てられる)。
    {
        const f32 data[2] = { 1.0f, 0.0f };
        TextureDesc td{};
        td.width = 1; td.height = 1;
        td.format = EFormat::R32G32_Float;
        td.initial_data = data;
        td.initial_data_size = sizeof(data);
        auto fb_r = CreateRhiTexture(device, td);
        if (fb_r.IsErr()) return Err<void>(fb_r.Error());
        _back_depth_fb = Move(fb_r.Value());
    }

    return Ok();
}

void RefractionShader::Shutdown() noexcept {
    _back_depth_fb.Reset();
    _back_depth = nullptr;
    _pipeline.Reset();
    _object_cb.Reset();
    _frame_cb.Reset();
    _ps.Reset();
    _vs.Reset();
}

void RefractionShader::SetFrame(const FMat4& view_projection, FVec3 camera_pos) noexcept {
    if (!_frame_cb) return;
    _vp  = view_projection;
    _eye = camera_pos;
    FrameCBLayout cb{};
    cb.view_proj  = _vp;
    cb.camera_pos = FVec4{_eye.x, _eye.y, _eye.z, 1.0f};
    cb.back_params = FVec4{_back_enabled ? 1.0f : 0.0f, _back_near, _back_far, 0};
    cb.screen_params = FVec4{
        _screen_w > 0 ? 1.0f / static_cast<f32>(_screen_w) : 0.0f,
        _screen_h > 0 ? 1.0f / static_cast<f32>(_screen_h) : 0.0f,
        0, 0};
    _frame_cb->Update(&cb, sizeof(cb));
}

void RefractionShader::SetBackDepth(IRhiTexture* back_depth, f32 near_z, f32 far_z,
                                     u32 screen_w, u32 screen_h) noexcept {
    _back_depth   = back_depth;
    _back_enabled = (back_depth != nullptr);
    _back_near    = near_z > 0.0f ? near_z : 0.1f;
    _back_far     = far_z  > _back_near ? far_z : (_back_near + 1.0f);
    _screen_w     = screen_w > 0 ? screen_w : 1;
    _screen_h     = screen_h > 0 ? screen_h : 1;
    // Frame CB を再 flush して back_params / screen_params を反映
    if (_frame_cb) {
        FrameCBLayout cb{};
        cb.view_proj  = _vp;
        cb.camera_pos = FVec4{_eye.x, _eye.y, _eye.z, 1.0f};
        cb.back_params = FVec4{_back_enabled ? 1.0f : 0.0f, _back_near, _back_far, 0};
        cb.screen_params = FVec4{1.0f / static_cast<f32>(_screen_w),
                                1.0f / static_cast<f32>(_screen_h), 0, 0};
        _frame_cb->Update(&cb, sizeof(cb));
    }
}

void RefractionShader::SetObject(const FMat4& model, f32 ior, f32 thickness,
                                 FVec3 tint, f32 roughness, f32 dispersion) noexcept {
    if (!_object_cb) return;
    const f32 r = roughness < 0.0f ? 0.0f : (roughness > 1.0f ? 1.0f : roughness);
    const f32 d = dispersion < 0.0f ? 0.0f : (dispersion > 1.0f ? 1.0f : dispersion);
    ObjectCBLayout cb{};
    cb.model    = model;
    cb.material = FVec4{ior < 1.0f ? 1.0f : ior,
                       thickness < 0.0f ? 0.0f : thickness, r, d};
    cb.tint     = FVec4{tint.x, tint.y, tint.z, 0};
    _object_cb->Update(&cb, sizeof(cb));
}

void RefractionShader::DrawMesh(IRhiCommandList& cmd, const GpuMesh& mesh,
                                const FMat4& model, IRhiTexture& background,
                                IRhiTexture& env, f32 ior, f32 thickness,
                                FVec3 tint, f32 roughness, f32 dispersion) noexcept {
    if (!_pipeline || !mesh.vertex_buffer || !mesh.index_buffer) return;
    SetObject(model, ior, thickness, tint, roughness, dispersion);

    cmd.SetPipeline(*_pipeline);
    cmd.SetConstantBuffer(0, *_frame_cb);
    cmd.SetConstantBuffer(1, *_object_cb);
    cmd.SetTexture(0, background);
    cmd.SetTexture(1, env);
    // Phase 35-3f: back_depth slot は SetBackDepth で渡されたテクスチャ、
    // 未設定なら 1x1 = 1.0 fallback (shader 側で「back_d>=0.9999 → scalar」)
    cmd.SetTexture(2, _back_depth ? *_back_depth : *_back_depth_fb);
    cmd.SetVertexBuffer(*mesh.vertex_buffer, mesh.vertex_stride);
    cmd.SetIndexBuffer(*mesh.index_buffer);
    cmd.DrawIndexed(mesh.index_count);
}

} // namespace acs
