// SPDX-License-Identifier: Apache-2.0
// スクリーンスペース屈折シェーダ実装
#include "render/RefractionShader.h"
#include "render/NormalMatrix.h"
#include "asset/MeshAsset.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

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
};

cbuffer Object : register(b1) {
    float4x4 model;
    float4   normal_row0;
    float4   normal_row1;
    float4   normal_row2;
    float4   material;       // x=ior, y=thickness, z=roughness, w=dispersion (Phase 35-3e)
    float4   tint;           // xyz=glass tint (吸収色), w=pad
    // Draw ごとに異なる background 寸法/env mip を shared Frame CB へ置かない。
    float4   screen_params;  // x=1/screen_w, y=1/screen_h, z=env mip count
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
    o.world_n = float3(
        v.nrm.x * normal_row0.x + v.nrm.y * normal_row1.x + v.nrm.z * normal_row2.x,
        v.nrm.x * normal_row0.y + v.nrm.y * normal_row1.y + v.nrm.z * normal_row2.y,
        v.nrm.x * normal_row0.z + v.nrm.y * normal_row1.z + v.nrm.z * normal_row2.z);
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
        // Pixel-space radius keeps frosted glass consistent across resolution
        // and aspect ratio. The old normalized 0.02 radius was ~38 px at 1080p.
        float2 filter_radius = screen_params.xy * (2.0 + roughness * roughness * 16.0);
        float3 sum = 0;
        [unroll]
        for (int t = 0; t < kTaps; ++t) {
            float ft = (float(t) + 0.5) / float(kTaps);
            float r2 = sqrt(ft);
            float a  = float(t) * kGoldenAngle;
            float2 off = float2(cos(a), sin(a)) * r2 * filter_radius;
            float rv = background.SampleLevel(background_sampler, saturate(refractUV_r + off), 0).r;
            float gv = background.SampleLevel(background_sampler, saturate(refractUV_g + off), 0).g;
            float bv = background.SampleLevel(background_sampler, saturate(refractUV_b + off), 0).b;
            sum += float3(rv, gv, bv);
        }
        refracted = sum / float(kTaps);
    }
    // Fade distortion before the refracted ray leaves the screen. Saturating
    // every displaced UV directly creates long edge-colour streaks.
    float2 screen_uv = v.pos.xy * screen_params.xy;
    float2 edge_distance = min(refractUV_g, 1.0 - refractUV_g);
    float edge_fade = saturate(min(edge_distance.x, edge_distance.y) * 40.0);
    float3 undistorted = background.SampleLevel(background_sampler, screen_uv, 0).rgb;
    refracted = lerp(undistorted, refracted, edge_fade);
    refracted *= tint.xyz;                               // ガラスの吸収色

    // --- Fresnel 反射 (環境マップ) ---
    float  NoV = saturate(dot(N, V));
    float  f0s = (ior - 1.0) / (ior + 1.0);
    float  F0  = f0s * f0s;
    float  F   = F0 + (1.0 - F0) * pow(saturate(1.0 - NoV), 5.0);
    // roughness で prefilter mip を選び、荒い面ほどボケた反射にする (常時シャープな
    // 「安い鏡」を回避)。screen_params.z = この draw の env mip 数。prefilter でない (mip=1) cubemap
    // なら LOD は 0 にクランプされ従来どおり。
    float  env_max_lod = max(screen_params.z - 1.0, 0.0);
    float  refl_lod    = roughness * env_max_lod;
    float3 reflected   = env.SampleLevel(env_sampler, reflect(-V, N), refl_lod).rgb;

    // 屈折と反射を Fresnel で混合
    return float4(lerp(refracted, reflected, F), 1.0);
}
)";

// CB レイアウト (HLSL と一致、float4 アライン)
struct FFrameCBLayout {
    FMat4 view_proj;
    FVec4 camera_pos;
    FVec4 back_params;     // x=enabled, y=near, z=far, w=pad
};

struct FObjectCbLayout {
    FMat4 model;
    FVec4 normal_row0;
    FVec4 normal_row1;
    FVec4 normal_row2;
    FVec4 material;       // x=ior, y=thickness, z=roughness, w=dispersion
    FVec4 tint;           // xyz=glass tint
    FVec4 screen_params;  // x=1/w, y=1/h, z=env mip count
};

// CB は 256B にアライン (DX12 制約)
template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

TResult<void> CRefractionShader::Init(IRhiDevice& device, EFormat rt_format,
                                    EFormat depth_format) noexcept {
    // === シェーダコンパイル ===
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kRefractionHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Refraction.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    m_Vs = Move(vs_r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kRefractionHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Refraction.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    m_Ps = Move(ps_r.Value());

    // === 定数バッファ ===
    FBufferDesc fcb{};
    fcb.size = CBSize<FFrameCBLayout>();
    fcb.usage = EBufferUsage::Uniform;
    fcb.cpu_writable = true;
    auto fcb_r = CreateRhiBuffer(device, fcb);
    if (fcb_r.IsErr()) return Err<void>(fcb_r.Error());
    m_FrameCb = Move(fcb_r.Value());

    FBufferDesc ocb{};
    ocb.size = CBSize<FObjectCbLayout>();
    ocb.usage = EBufferUsage::Uniform;
    ocb.cpu_writable = true;
    for (u32 i = 0; i < kObjectCbRing; ++i) {
        auto ocb_r = CreateRhiBuffer(device, ocb);
        if (ocb_r.IsErr()) return Err<void>(ocb_r.Error());
        m_ObjectCbs[i] = Move(ocb_r.Value());
    }
    m_ObjectCbCursor = 0;
    m_CurrentObjectCb = kObjectCbRing;

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
    pd.blend_mode    = EBlendMode::Opaque;   // SS 屈折は背景を曲げて不透明描画する
    pd.cbuffer_slots = 2;     // b0=Frame, b1=Object
    pd.texture_slots = 3;     // t0=background, t1=env, t2=back_depth
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
    pd.vertex_stride = sizeof(FMeshVertex);
    pd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0  };
    pd.layout[1] = { "NORMAL",   0, EFormat::R32G32B32_Float, 16 }; // FVec3 はアライン 16
    pd.layout[2] = { "TEXCOORD", 0, EFormat::R32G32_Float,    32 };
    pd.layout_count = 3;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    m_Pipeline = Move(pl_r.Value());

    // back_depth fallback: 1x1 R32G32_Float texture。.r = 1.0 を
    // 入れ、shader の「back_d >= 0.9999 → スカラー fallback」分岐に必ず hit
    // させる (R32_Float が EFormat 未定義のため 2ch を採用、.g は捨てられる)。
    {
        const f32 data[2] = { 1.0f, 0.0f };
        FTextureDesc td{};
        td.width = 1; td.height = 1;
        td.format = EFormat::R32G32_Float;
        td.initial_data = data;
        td.initial_data_size = sizeof(data);
        auto fb_r = CreateRhiTexture(device, td);
        if (fb_r.IsErr()) return Err<void>(fb_r.Error());
        m_BackDepthFb = Move(fb_r.Value());
    }

    return Ok();
}

void CRefractionShader::Shutdown() noexcept {
    m_BackDepthFb.Reset();
    m_BackDepth = nullptr;
    m_Pipeline.Reset();
    for (auto& cb : m_ObjectCbs) cb.Reset();
    m_ObjectCbCursor = 0;
    m_CurrentObjectCb = kObjectCbRing;
    m_FrameCb.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
}

void CRefractionShader::SetFrame(const FMat4& view_projection, FVec3 camera_pos,
                                 u32 screen_w, u32 screen_h) noexcept {
    m_ObjectCbCursor = 0;
    m_CurrentObjectCb = kObjectCbRing;
    m_Vp  = view_projection;
    m_Eye = camera_pos;
    if (screen_w > 0) m_ScreenW = screen_w;
    if (screen_h > 0) m_ScreenH = screen_h;
    if (!m_FrameCb) return;
    FFrameCBLayout cb{};
    cb.view_proj  = m_Vp;
    cb.camera_pos = FVec4{m_Eye.x, m_Eye.y, m_Eye.z, 1.0f};
    cb.back_params = FVec4{m_bBackEnabled ? 1.0f : 0.0f, m_BackNear, m_BackFar, 0};
    m_FrameCb->Update(&cb, sizeof(cb));
}

void CRefractionShader::SetBackDepth(IRhiTexture* back_depth, f32 near_z, f32 far_z,
                                     u32 screen_w, u32 screen_h) noexcept {
    m_BackDepth   = back_depth;
    m_bBackEnabled = (back_depth != nullptr);
    m_BackNear    = near_z > 0.0f ? near_z : 0.1f;
    m_BackFar     = far_z  > m_BackNear ? far_z : (m_BackNear + 1.0f);
    m_ScreenW     = screen_w > 0 ? screen_w : 1;
    m_ScreenH     = screen_h > 0 ? screen_h : 1;
    // Frame CB を再 flush して back_params を反映。screen/env 情報は draw 専用
    // Object CB に置き、先行 draw の Frame CB を後続 draw から上書きしない。
    if (m_FrameCb) {
        FFrameCBLayout cb{};
        cb.view_proj  = m_Vp;
        cb.camera_pos = FVec4{m_Eye.x, m_Eye.y, m_Eye.z, 1.0f};
        cb.back_params = FVec4{m_bBackEnabled ? 1.0f : 0.0f, m_BackNear, m_BackFar, 0};
        m_FrameCb->Update(&cb, sizeof(cb));
    }
}

void CRefractionShader::SetObject(const FMat4& model, f32 ior, f32 thickness,
                                 FVec3 tint, f32 roughness, f32 dispersion,
                                 u32 env_mip_levels) noexcept {
    if (m_ObjectCbCursor >= kObjectCbRing) {
        if (m_ObjectCbCursor == kObjectCbRing) {
            ACS_LOG_WARN("CRefractionShader: per-frame draw limit (%u) exceeded; "
                         "remaining refraction draws are skipped", kObjectCbRing);
            ++m_ObjectCbCursor;
        }
        m_CurrentObjectCb = kObjectCbRing;
        return;
    }
    m_CurrentObjectCb = m_ObjectCbCursor++;
    IRhiBuffer* object_cb = m_ObjectCbs[m_CurrentObjectCb].Get();
    if (!object_cb) {
        m_CurrentObjectCb = kObjectCbRing;
        return;
    }
    const f32 r = roughness < 0.0f ? 0.0f : (roughness > 1.0f ? 1.0f : roughness);
    const f32 d = dispersion < 0.0f ? 0.0f : (dispersion > 1.0f ? 1.0f : dispersion);
    FObjectCbLayout cb{};
    cb.model    = model;
    const FMat4 normal_matrix = MakeSafeNormalMatrix(model);
    cb.normal_row0 = FVec4{normal_matrix.m[0][0], normal_matrix.m[0][1],
                           normal_matrix.m[0][2], 0};
    cb.normal_row1 = FVec4{normal_matrix.m[1][0], normal_matrix.m[1][1],
                           normal_matrix.m[1][2], 0};
    cb.normal_row2 = FVec4{normal_matrix.m[2][0], normal_matrix.m[2][1],
                           normal_matrix.m[2][2], 0};
    cb.material = FVec4{ior < 1.0f ? 1.0f : ior,
                       thickness < 0.0f ? 0.0f : thickness, r, d};
    cb.tint     = FVec4{tint.x, tint.y, tint.z, 0};
    cb.screen_params = FVec4{
        m_ScreenW > 0 ? 1.0f / static_cast<f32>(m_ScreenW) : 0.0f,
        m_ScreenH > 0 ? 1.0f / static_cast<f32>(m_ScreenH) : 0.0f,
        static_cast<f32>(env_mip_levels > 0 ? env_mip_levels : 1u), 0};
    object_cb->Update(&cb, sizeof(cb));
}

void CRefractionShader::DrawMesh(IRhiCommandList& cmd, const FGpuMesh& mesh,
                                const FMat4& model, IRhiTexture& background,
                                IRhiTexture& env, f32 ior, f32 thickness,
                                FVec3 tint, f32 roughness, f32 dispersion) noexcept {
    // SetBackDepth は optional。通常の DrawMesh 経路でも実際に sample する
    // background の寸法を必ず採用し、rough blur を 1x1 texel size に退行させない。
    m_ScreenW = background.Width() > 0 ? background.Width() : 1;
    m_ScreenH = background.Height() > 0 ? background.Height() : 1;
    if (!m_Pipeline || !m_FrameCb || !mesh.vertex_buffer || !mesh.index_buffer) return;
    SetObject(model, ior, thickness, tint, roughness, dispersion, env.MipLevels());
    IRhiBuffer* object_cb = PerObjectCB();
    if (!object_cb) return;

    cmd.SetPipeline(*m_Pipeline);
    cmd.SetConstantBuffer(0, *m_FrameCb);
    cmd.SetConstantBuffer(1, *object_cb);
    cmd.SetTexture(0, background);
    cmd.SetTexture(1, env);
    // back_depth slot は SetBackDepth で渡されたテクスチャ、
    // 未設定なら 1x1 = 1.0 fallback (shader 側で「back_d>=0.9999 → scalar」)
    cmd.SetTexture(2, m_BackDepth ? *m_BackDepth : *m_BackDepthFb);
    cmd.SetVertexBuffer(*mesh.vertex_buffer, mesh.vertex_stride);
    cmd.SetIndexBuffer(*mesh.index_buffer);
    cmd.DrawIndexed(mesh.index_count);
}

} // namespace acs
