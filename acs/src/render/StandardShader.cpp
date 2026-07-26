// SPDX-License-Identifier: Apache-2.0
// 標準ライティングシェーダ実装
#include "render/StandardShader.h"
#include "asset/MeshAsset.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <cstring>

namespace acs {

namespace {

/**
 * 標準ライティングシェーダの HLSL ソース (VS + PS、row-major で FMat4 をそのまま受け取る)。
 *
 * @details
 * 最大 4 灯の有向光源 + 4 灯の点光源 + 環境光 + Blinn-Phong スペキュラ + PCSS シャドウマップ。
 */
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
// 命名規約: <texture>m_Sampler。Diligent の CombinedSamplerSuffix=m_Sampler と一致
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

float AcsStandardSrgbToLinearChannel(float value) {
    float safe_value = saturate(value);
    return safe_value <= 0.04045
        ? safe_value / 12.92
        : pow(abs((safe_value + 0.055) / 1.055), 2.4);
}

float3 AcsStandardDecodeAlbedo(float3 value) {
    return float3(
        AcsStandardSrgbToLinearChannel(value.r),
        AcsStandardSrgbToLinearChannel(value.g),
        AcsStandardSrgbToLinearChannel(value.b));
}

// シャドウ係数: 1=完全に光が当たる、0=影、中間で半影。
//
// PCSS (Percentage-Closer Soft Shadow、Fernando 2005)。
// 1) Vogel disk 16 tap の blocker search で receiver 周囲の実遮蔽点 avg depth を求める
// 2) penumbra width = (receiver - blocker_avg) / blocker_avg * light_size
//    遮蔽点が receiver から遠いほど影が大きく柔らかくなる物理的挙動
// 3) penumbra サイズで重み付き Vogel disk 24 tap PCF
// 初期値 1 の単一 return にして、無効・範囲外・blocker 無しを安全に扱う。
//
// shadow_params.w = filter_radius (0=hard PCF、1.0=PCSS 標準、>1 で更に柔らか)
float InterleavedGradientNoise(float2 pixel) {
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float2 VogelDiskSample(int sample_index, int sample_count, float angle) {
    const float golden_angle = 2.39996323;
    float r = sqrt((float(sample_index) + 0.5) / float(sample_count));
    float theta = float(sample_index) * golden_angle + angle;
    float s, c;
    sincos(theta, s, c);
    return float2(c, s) * r;
}

float ComputeShadow(float3 world_p, float3 world_n, float3 light_to_surface) {
    float shadow_result = 1.0;
    if (shadow_params.y >= 0.5) {
        float4 light_clip =
            mul(float4(world_p, 1.0), light_view_proj);
        if (light_clip.w > 1e-5) {
            float3 light_ndc = light_clip.xyz / light_clip.w;
            bool inside_shadow_map =
                light_ndc.x >= -1.0 && light_ndc.x <= 1.0 &&
                light_ndc.y >= -1.0 && light_ndc.y <= 1.0 &&
                light_ndc.z >=  0.0 && light_ndc.z <= 1.0;
            if (inside_shadow_map) {
                float2 uv = float2(
                    light_ndc.x * 0.5 + 0.5,
                    -light_ndc.y * 0.5 + 0.5);
                float my_d = light_ndc.z;
                float ts = max(abs(shadow_params.z), 1e-7);
                float kFilt = max(shadow_params.w, 0.0);

    // Grazing angle でだけ receiver bias を増やす。正面では基準 bias を保つため
    // contact shadow の浮きを抑えつつ、斜面の acne を軽減できる。
    float ndotl = saturate(dot(normalize(world_n), normalize(light_to_surface)));
    float bias = max(shadow_params.x, 0.0)
               * lerp(1.0, 2.5, 1.0 - ndotl);
    float angle = InterleavedGradientNoise(floor(uv / max(ts, 1e-7))) * 6.28318531;
    float2 min_uv = float2(ts * 0.5, ts * 0.5);
    float2 max_uv = 1.0 - min_uv;

    // ---- 1) Blocker search ----
    float blocker_sum = 0;
    int   blocker_cnt = 0;
    const int kBlockerSamples = 16;
    float search_r = ts * lerp(3.0, 5.0, saturate(kFilt));
    [unroll]
    for (int blocker_sample_index = 0;
         blocker_sample_index < kBlockerSamples;
         ++blocker_sample_index) {
        float2 off = VogelDiskSample(
            blocker_sample_index, kBlockerSamples, angle) * search_r;
        float sd = shadow_map.SampleLevel(shadow_map_sampler, clamp(uv + off, min_uv, max_uv), 0).r;
        if (sd + bias < my_d) {
            blocker_sum += sd;
            blocker_cnt += 1;
        }
    }
    if (blocker_cnt == kBlockerSamples) {
        shadow_result = 0.0;
    } else if (blocker_cnt > 0) {
    float blocker_avg = blocker_sum / float(blocker_cnt);

    // ---- 2) Penumbra width ----
    // light_size_uv = 0.01 をハードコード、kFilt で全体スケーリング。
    // FPbrShader と同じ係数で 27_HelloShowcase 等の見た目に整合。
    float pcss_penumbra = max((my_d - blocker_avg) / max(blocker_avg, 1e-3), 0.0);
    float filter_r = max(pcss_penumbra * 0.01 * kFilt, ts * 1.25);
    // 極端な receiver/blocker 比で隣接物へ影が漏れるのを防ぐ。
    filter_r = min(filter_r, ts * lerp(8.0, 24.0, saturate(kFilt)));

    // ---- 3) 半影サイズに応じた重み付き Vogel PCF ----
    float lit = 0;
    float weight_sum = 0;
    const int kPcfSamples = 24;
    [unroll]
    for (int pcf_sample_index = 0;
         pcf_sample_index < kPcfSamples;
         ++pcf_sample_index) {
        float2 disk =
            VogelDiskSample(pcf_sample_index, kPcfSamples, angle);
        float weight = 1.0 - 0.35 * length(disk);  // 中央寄りの滑らかな disk kernel
        float sd = shadow_map.SampleLevel(
            shadow_map_sampler, clamp(uv + disk * filter_r, min_uv, max_uv), 0).r;
        lit += ((sd + bias >= my_d) ? 1.0 : 0.0) * weight;
        weight_sum += weight;
    }
    shadow_result = lit / max(weight_sum, 1e-4);
    }
            }
        }
    }
    return shadow_result;
}

float4 PSMain(VSOut v) : SV_TARGET {
    float3 N = normalize(v.world_n);
    float3 V = normalize(camera_pos.xyz - v.world_p);

    // LDR color assets are uploaded as linear UNORM resources containing their
    // original sRGB bytes. Decode only the sampled color; base_color is linear.
    float3 albedo_texel_linear = AcsStandardDecodeAlbedo(
        albedo.Sample(albedo_sampler, v.uv).rgb);
    float3 albedo_rgb = albedo_texel_linear * base_color.xyz;

    // 環境光
    float3 col = ambient.xyz * albedo_rgb;
    int active_dir_count = (int)ambient.w;

    // シャドウ係数（最初の dir light のみ適用）。1 pixel につき 1 回だけ評価する。
    float main_shadow = (active_dir_count > 0)
        ? ComputeShadow(v.world_p, N, light_dir[0].xyz)
        : 1.0;

    // 1) 有向光源（Lambert + Blinn-Phong）。先頭光源にのみシャドウを乗算
    [unroll]
    for (int dir_light_index = 0;
         dir_light_index < ACS_MAX_DIR_LIGHTS;
         ++dir_light_index) {
        if (dir_light_index >= active_dir_count) break;
        float3 L = normalize(light_dir[dir_light_index].xyz);
        float  diff = saturate(dot(N, L));
        float3 H = normalize(L + V);
        float dir_spec_base = abs(saturate(dot(N, H)));
        float spec =
            pow(abs(dir_spec_base), max(material.y, 1.0)) * material.x;
        float k = (dir_light_index == 0) ? main_shadow : 1.0;
        col += light_color[dir_light_index].xyz
             * (albedo_rgb * diff + spec) * k;
    }

    // 2) 点光源（距離による減衰付き）
    int active_point_count = (int)point_count_pad.x;
    [unroll]
    for (int point_light_index = 0;
         point_light_index < ACS_MAX_POINT_LIGHTS;
         ++point_light_index) {
        if (point_light_index >= active_point_count) break;
        float3 to_light =
            point_pos_range[point_light_index].xyz - v.world_p;
        float  dist = length(to_light);
        float  rng =
            max(point_pos_range[point_light_index].w, 0.0001);
        if (dist >= rng) continue;
        float3 L = to_light / dist;
        float  att = 1.0 - dist / rng;
        att = att * att;                                  // smooth falloff
        float  diff = saturate(dot(N, L)) * att;
        float3 H = normalize(L + V);
        float point_spec_base = abs(saturate(dot(N, H)));
        float spec = pow(abs(point_spec_base), max(material.y, 1.0))
                   * material.x * att;
        col += point_color[point_light_index].xyz
             * (albedo_rgb * diff + spec);
    }

    return float4(col, base_color.w);
}
)";

/** 有向光源の最大数 (HLSL の ACS_MAX_DIR_LIGHTS と一致)。 */
constexpr u32 kMaxDirLights   = 4;

/** 点光源の最大数 (HLSL の ACS_MAX_POINT_LIGHTS と一致)。 */
constexpr u32 kMaxPointLights = 4;

/**
 * フレーム単位の定数バッファのレイアウト (HLSL の cbuffer Frame と一致、float4 アライン)。
 */
struct FFrameCBLayout {
    /** view-projection 行列。 */
    FMat4 view_proj;

    /** 視点ワールド座標 (xyz=eye, w=pad)。 */
    FVec4 camera_pos;

    /** 環境光と有向光源数 (xyz=ambient, w=dir_count)。 */
    FVec4 ambient;

    /** 点光源数 (x=point_count)。 */
    FVec4 point_count_pad;

    /** 各有向光源の方向 (xyz=光へ向く方向, w=pad)。 */
    FVec4 light_dir  [kMaxDirLights];

    /** 各有向光源の色 (xyz=color, w=pad)。 */
    FVec4 light_color[kMaxDirLights];

    /** 各点光源の位置と範囲 (xyz=world pos, w=range)。 */
    FVec4 point_pos_range[kMaxPointLights];

    /** 各点光源の色 (xyz=color, w=pad)。 */
    FVec4 point_color    [kMaxPointLights];

    /** シャドウマップ投影用の light view-projection 行列。 */
    FMat4 light_view_proj;

    /** シャドウパラメータ (x=bias, y=enabled, z=texel_size, w=filter_radius)。 */
    FVec4 shadow_params;
};

/**
 * オブジェクト単位の定数バッファのレイアウト (HLSL の cbuffer Object と一致)。
 */
struct FObjectCbLayout {
    /** モデル (world) 行列。 */
    FMat4 model;

    /** ベースカラー (xyz=color, w=alpha)。 */
    FVec4 base_color;

    /** マテリアル (x=specular_strength, y=shininess, zw=pad)。 */
    FVec4 material;
};

/**
 * 定数バッファサイズを 256 バイト境界に切り上げる (DX12 制約)。
 *
 * @tparam T 定数バッファのレイアウト型。
 * @return sizeof(T) を 256 の倍数に切り上げたバイト数。
 */
template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

/** VS/PS のコンパイル、定数バッファ・白テクスチャ・パイプラインを生成する。 */
TResult<void> FStandardShader::Init(IRhiDevice& device, EFormat rt_format, EFormat depth_format) noexcept {
    Shutdown();

    // シェーダコンパイル
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kStandardHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Standard.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    m_Vs = Move(vs_r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kStandardHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Standard.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    m_Ps = Move(ps_r.Value());

    // 定数バッファ
    FBufferDesc fcb{};
    fcb.size = CBSize<FFrameCBLayout>();
    fcb.usage = EBufferUsage::Uniform;
    fcb.cpu_writable = true;
    auto fcb_r = CreateRhiBuffer(device, fcb);
    if (fcb_r.IsErr()) return Err<void>(fcb_r.Error());
    m_FrameCb = Move(fcb_r.Value());

    m_ResourceDevice = &device;
    if (!EnsureObjectCapacity(kInitialObjectBufferCapacity)) {
        const FErrorCode error = ACS_ERR(
            Render, 381, "Standard initial object-CB pool allocation failed");
        Shutdown();
        return error;
    }
    m_ObjectCbCursor = 0u;
    // Legacy manual paths may query/bind before their first SetObject. Slot 0 is
    // exactly the slot the first SetObject will populate, so this remains safe.
    m_CurrentObjectCb = 0u;
    m_FrameCapacityReady = true;
    m_ObjectCapacityFailureLogged = false;

    // 1×1 白テクスチャ
    const u8 white_pixel[4] = { 255, 255, 255, 255 };
    FTextureDesc td{};
    td.width = 1; td.height = 1;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = white_pixel;
    td.initial_data_size = 4;
    auto wt_r = CreateRhiTexture(device, td);
    if (wt_r.IsErr()) return Err<void>(wt_r.Error());
    m_White = Move(wt_r.Value());

    // パイプライン
    FPipelineDesc pd{};
    pd.vs = m_Vs.Get();
    pd.ps = m_Ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = depth_format;
    pd.depth_test    = depth_format != EFormat::Unknown;
    pd.depth_write   = true;
    pd.cull_mode     = ECullMode::Back;
    pd.cbuffer_slots = 2;     // b0=Frame, b1=Object
    pd.texture_slots = 2;     // t0=albedo, t1=shadow_map
    pd.cbuffer_names[0] = "Frame";
    pd.cbuffer_names[1] = "Object";
    pd.texture_names[0] = "albedo";
    pd.texture_names[1] = "shadow_map";
    pd.static_sampler_count = 2;
    // albedo は Linear。異方性 (Anisotropic) は mip チェーンが無いと実質 bilinear で
    // 効果が出ないため、アルベドの自動 mip 生成を入れるまでは Linear のままにする。
    pd.static_samplers[0].filter    = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Wrap;
    pd.static_samplers[0].address_v = ESamplerAddress::Wrap;
    pd.static_samplers[1].filter    = ESamplerFilter::Linear;
    pd.static_samplers[1].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[1].address_v = ESamplerAddress::Clamp;
    pd.vertex_stride = sizeof(FMeshVertex);
    pd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0  };
    pd.layout[1] = { "NORMAL",   0, EFormat::R32G32B32_Float, 16 }; // FVec3 はアライン 16
    pd.layout[2] = { "TEXCOORD", 0, EFormat::R32G32_Float,    32 };
    pd.layout_count = 3;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    m_Pipeline = Move(pl_r.Value());

    return Ok();
}

/** GPU リソースを解放する。 */
void FStandardShader::Shutdown() noexcept {
    m_Pipeline.Reset();
    m_White.Reset();
    m_ObjectCbs.ReleaseStorage();
    m_ObjectCbCursor = 0u;
    m_CurrentObjectCb = kInvalidObjectBuffer;
    m_FrameCapacityReady = false;
    m_ObjectCapacityFailureLogged = false;
    m_ResourceDevice = nullptr;
    m_FrameCb.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
}

bool FStandardShader::EnsureObjectCapacity(
    u32 required_object_draws) noexcept {
    if (required_object_draws == kInvalidObjectBuffer) return false;
    if (!m_ResourceDevice) return false;
    if (required_object_draws <= m_ObjectCbs.Size()) return true;

    u32 target = static_cast<u32>(m_ObjectCbs.Size());
    if (target < kInitialObjectBufferCapacity)
        target = kInitialObjectBufferCapacity;
    while (target < required_object_draws) {
        const u32 growth = target > 1u ? target / 2u : 1u;
        if (target > kInvalidObjectBuffer - growth) {
            target = required_object_draws;
        } else {
            target += growth;
        }
    }

    if (!m_ObjectCbs.TryReserve(target)) return false;
    while (m_ObjectCbs.Size() < target) {
        FBufferDesc description{};
        description.size = CBSize<FObjectCbLayout>();
        description.usage = EBufferUsage::Uniform;
        description.cpu_writable = true;
        auto created = CreateRhiBuffer(*m_ResourceDevice, description);
        if (created.IsErr())
            return m_ObjectCbs.Size() >= required_object_draws;
        if (!m_ObjectCbs.TryPushBack(Move(created.Value())))
            return m_ObjectCbs.Size() >= required_object_draws;
    }
    return true;
}

bool FStandardShader::BeginFrame(u32 required_object_draws) noexcept {
    m_ObjectCbCursor = 0u;
    m_CurrentObjectCb =
        m_ObjectCbs.IsEmpty() ? kInvalidObjectBuffer : 0u;
    m_ObjectCapacityFailureLogged = false;
    m_FrameCapacityReady = EnsureObjectCapacity(required_object_draws);
    if (!m_FrameCapacityReady)
        m_CurrentObjectCb = kInvalidObjectBuffer;
    return m_FrameCapacityReady;
}

/** 単一の有向光源を組み立てて SetLights に委譲する。 */
void FStandardShader::SetFrame(const FMat4& vp, FVec3 cam, FVec3 light_dir,
                              FVec3 light_color, FVec3 ambient) noexcept {
    FDirLight one;
    one.direction = light_dir;
    one.color     = light_color;
    SetLights(vp, cam, &one, 1, ambient);
}

/** 有向光源群・カメラ・環境光を保持し、フレーム CB を更新する。 */
void FStandardShader::SetLights(const FMat4& vp, FVec3 cam,
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

/** 点光源群を保持し、フレーム CB を更新する。 */
void FStandardShader::SetPointLights(const FPointLight* lights, u32 count) noexcept {
    if (count > kMaxPointLights) count = kMaxPointLights;
    m_PointCount = count;
    for (u32 i = 0; i < count; ++i) m_PointLights[i] = lights[i];
    FlushFrameCB();
}

/** シャドウマップと light view-projection・bias・filter を保持し、フレーム CB を更新する。 */
void FStandardShader::SetShadowMap(IRhiTexture* tex, const FMat4& light_vp,
                                   f32 bias, f32 filter_radius) noexcept {
    m_ShadowTex    = tex;
    m_LightVp      = light_vp;
    m_ShadowBias   = bias;
    m_ShadowFilter = filter_radius;
    FlushFrameCB();
}

/** 保持中のライト・カメラ・シャドウ状態をフレーム定数バッファに書き込む。 */
void FStandardShader::FlushFrameCB() noexcept {
    if (!m_FrameCb) return;
    FFrameCBLayout cb{};
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
    cb.light_view_proj = m_LightVp;
    // shadow_params: x=bias, y=enabled (0/1), z=texel_size (UV)、w=filter_radius
    // atlas の場合も height は 1 cascade の解像度なので、実テクスチャから取得すれば
    // 1024/2048/4096 のいずれでも blocker search と PCF 半径が正しい texel 単位になる。
    const u32 shadow_size = m_ShadowTex ? m_ShadowTex->Height() : 0u;
    const f32 shadow_texel = shadow_size > 0
        ? 1.0f / static_cast<f32>(shadow_size)
        : 0.0f;
    cb.shadow_params = FVec4{
        m_ShadowBias,
        m_ShadowTex ? 1.0f : 0.0f,
        shadow_texel,
        m_ShadowFilter
    };
    m_FrameCb->Update(&cb, sizeof(cb));
}

/** モデル行列・ベースカラー・マテリアルをオブジェクト定数バッファに書き込む。 */
bool FStandardShader::SetObject(const FMat4& model, FVec3 base_color,
                               f32 specular_strength, f32 shininess) noexcept {
    if (!m_FrameCapacityReady ||
        m_ObjectCbCursor == kInvalidObjectBuffer ||
        (m_ObjectCbCursor >= m_ObjectCbs.Size() &&
         !EnsureObjectCapacity(m_ObjectCbCursor + 1u))) {
        if (!m_ObjectCapacityFailureLogged) {
            ACS_LOG_WARN(
                "FStandardShader: unable to grow per-frame object-CB pool "
                "(required=%u, retained=%zu); remaining draws are skipped",
                m_ObjectCbCursor == kInvalidObjectBuffer
                    ? kInvalidObjectBuffer : m_ObjectCbCursor + 1u,
                m_ObjectCbs.Size());
            m_ObjectCapacityFailureLogged = true;
        }
        m_FrameCapacityReady = false;
        m_CurrentObjectCb = kInvalidObjectBuffer;
        return false;
    }
    m_CurrentObjectCb = m_ObjectCbCursor++;
    IRhiBuffer* object_cb = m_ObjectCbs[m_CurrentObjectCb].Get();
    if (!object_cb) {
        m_FrameCapacityReady = false;
        m_CurrentObjectCb = kInvalidObjectBuffer;
        return false;
    }
    FObjectCbLayout cb{};
    cb.model      = model;
    cb.base_color = FVec4{base_color.x, base_color.y, base_color.z, 1.0f};
    cb.material   = FVec4{specular_strength, shininess, 0, 0};
    object_cb->Update(&cb, sizeof(cb));
    return true;
}

/** オブジェクト CB を更新し、パイプライン・バッファ・テクスチャを束ねてメッシュを描画する。 */
void FStandardShader::DrawMesh(IRhiCommandList& cmd,
                              const FGpuMesh& mesh,
                              const FMat4& model,
                              FVec3 base_color,
                              f32  specular_strength,
                              f32  shininess,
                              IRhiTexture* albedo) noexcept {
    if (!m_Pipeline || !m_FrameCb || !m_White ||
        !mesh.vertex_buffer || !mesh.index_buffer) return;
    if (!SetObject(model, base_color, specular_strength, shininess)) return;
    IRhiBuffer* object_cb = PerObjectCB();
    if (!object_cb) return;

    cmd.SetPipeline(*m_Pipeline);
    cmd.SetConstantBuffer(0, *m_FrameCb);
    cmd.SetConstantBuffer(1, *object_cb);
    cmd.SetTexture(0, albedo ? *albedo : *m_White);
    cmd.SetTexture(1, *ShadowTextureOrDefault());
    cmd.SetVertexBuffer(*mesh.vertex_buffer, mesh.vertex_stride);
    cmd.SetIndexBuffer(*mesh.index_buffer);
    cmd.DrawIndexed(mesh.index_count);
}

} // namespace acs
