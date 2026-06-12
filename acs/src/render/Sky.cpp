// SPDX-License-Identifier: Apache-2.0
// 手続き生成スカイ実装
#include "render/Sky.h"
#include "math/Camera.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <cstring>

namespace acs {

namespace {

/** スカイの HLSL ソース (フルスクリーン三角形 + 視線方向ベースの空と太陽。VB 不要)。 */
const char* kSkyHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer FSky : register(b0) {
    float4x4 inv_view_proj;       // 画面 NDC → ワールドへの逆変換
    float4   camera_pos;          // xyz=eye
    float4   sun_dir;             // xyz=方向 (camera→sun)
    float4   sun_color;           // xyz=色
    float4   sun_params;          // x=radius (1-cos), y=glow, zw=pad
    float4   zenith_color;
    float4   horizon_color;
    float4   ground_color;
    float4   cloud_params0;       // x=coverage(0..1), y=density(sharpness), z=time, w=enabled(0/1)
    float4   cloud_params1;       // xyz=cloud_color, w=wind_speed
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 ndc : TEXCOORD0;       // -1..+1 の 2D NDC
};

VSOut VSMain(uint id : SV_VertexID) {
    // 大きな三角形を 1 枚張ってフルスクリーンを覆う
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.ndc = uv * 2.0 - 1.0;
    // D3D の Y は下が +、ndc.y を反転して上から下に統一
    o.pos = float4(o.ndc.x, -o.ndc.y, 1.0, 1.0);    // z=1 (far)
    return o;
}

// ---- 雲用の value noise + FBM ----------------------------------------------
// 軽量なハッシュ value noise を 6 octave 重ね、octave ごとに座標を回転させて
// 軸に揃ったタイリングを崩す。gradient/simplex より単純だが、回転 + 多 octave で
// 十分に有機的な雲になる。
float Hash2(float2 p) {
    p = frac(p * float2(127.1, 311.7));
    p += dot(p, p + 34.23);
    return frac(p.x * p.y);
}
float ValueNoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);          // smoothstep 補間で格子段差を消す
    float a = Hash2(i + float2(0.0, 0.0));
    float b = Hash2(i + float2(1.0, 0.0));
    float c = Hash2(i + float2(0.0, 1.0));
    float d = Hash2(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}
float Fbm(float2 p) {
    float sum = 0.0;
    float amp = 0.5;
    const float2x2 rot = float2x2(0.80, -0.60, 0.60, 0.80);   // ~37deg 回転
    [unroll]
    for (int i = 0; i < 6; ++i) {
        sum += amp * ValueNoise(p);
        p    = mul(rot, p) * 2.02;               // lacunarity ~2 + 回転
        amp *= 0.5;                              // gain 0.5
    }
    return sum;
}

// IGN ベースの dither (8-bit 量子化前にバンディングを消す)。
float SkyDither(float2 pix, float t) {
    pix += t * float2(5.588238, 1.715728);
    return frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715))));
}

float4 PSMain(VSOut v) : SV_TARGET {
    // 1) NDC（z=1）から逆 VP でワールド座標を求める
    float4 wp = mul(float4(v.ndc.x, -v.ndc.y, 1.0, 1.0), inv_view_proj);
    wp.xyz /= wp.w;
    float3 dir   = normalize(wp.xyz - camera_pos.xyz);
    float3 sundn = normalize(sun_dir.xyz);

    // 2) 高さ角でグラデ。dir.y = 0 が地平線、+1 が天頂、-1 が真下。
    //    指数を 0.5 に緩めて天頂までの遷移を滑らかにする。
    float t = dir.y;
    float3 sky;
    if (t >= 0.0) {
        sky = lerp(horizon_color.xyz, zenith_color.xyz, pow(saturate(t), 0.5));
    } else {
        sky = lerp(horizon_color.xyz, ground_color.xyz, pow(saturate(-t), 0.5));
    }

    // 2.5) 太陽方向の地平線グロー (Mie 前方散乱の簡易ローブ): 地平線付近 + 太陽方位で
    //      暖色を盛り、のっぺりした 2-stop グラデを大気らしくする。
    float sun_d      = saturate(dot(dir, sundn));
    float horizonBnd = exp(-abs(t) * 6.0);             // 地平線に集中
    float glow       = pow(sun_d, 4.0) * horizonBnd;
    sky += sun_color.xyz * glow * 0.6;

    // 3) 太陽ディスク + ハロー
    float c   = sun_d;
    float ang = 1.0 - c;    // 0 = 太陽の中心
    if (ang < sun_params.x) {
        sky = sun_color.xyz;
    } else if (ang < sun_params.y) {
        float k = 1.0 - smoothstep(sun_params.x, sun_params.y, ang);
        sky = lerp(sky, sun_color.xyz, k);
    }

    // 4) 手続き的な雲 (地平線より上のみ)。視線を高さ 1 の雲平面に投影して FBM を引く。
    if (cloud_params0.w >= 0.5 && dir.y > 0.004) {
        float  time     = cloud_params0.z;
        float  wind     = cloud_params1.w;
        // 平面投影。地平線方向 (dir.y 小) で遠近感が出るよう dir.y を少し持ち上げて
        // uv の発散を抑える (地平線でも雲が潰れず層状に見える)。
        float2 uv       = (dir.xz / (dir.y + 0.12)) * 1.1;
        uv += float2(time * wind * 0.02, time * wind * 0.013);

        float coverage  = saturate(cloud_params0.x);
        float density   = max(cloud_params0.y, 0.1);
        float n         = Fbm(uv);
        // coverage で remap: coverage が高いほど薄い部分も雲になる
        float clouds    = saturate((n - (1.0 - coverage)) / max(coverage, 0.001));
        clouds          = pow(clouds, density);
        // 地平線のすぐ上から雲を出し、天頂へ向けてしっかり乗せる。
        float hFade     = smoothstep(0.0, 0.10, dir.y);
        clouds         *= hFade;

        // 簡易ライティング: 雲の濃い所を影色、薄い縁を明色 + 太陽方向で明るく。
        // contrast を上げて空との分離をはっきりさせる。
        float3 litCol   = lerp(cloud_params1.xyz, sun_color.xyz, sun_d * 0.5);
        float3 shadow   = cloud_params1.xyz * 0.45;
        float3 cloudCol = lerp(shadow, litCol, saturate(n * 1.4));
        sky = lerp(sky, cloudCol, clouds);
    }

    // 5) Dither: 8-bit 出力時のグラデ縞を消す (HDR 出力時は ±1/255 で実質無影響)。
    //    d2 は軸別オフセット + 別の時間位相で d1 と独立化し、足して TPDF にする。
    float d1 = SkyDither(v.pos.xy, cloud_params0.z);
    float d2 = SkyDither(v.pos.xy + float2(113.0, 71.0), cloud_params0.z * 0.37 + 0.5);
    sky += (d1 + d2 - 1.0) * (1.0 / 255.0);

    return float4(sky, 1.0);
}
)";

/**
 * スカイ定数バッファのレイアウト (HLSL の cbuffer FSky と一致)。
 */
struct SkyCB {
    /** 画面 NDC からワールドへの逆 view-projection。 */
    FMat4 inv_view_proj;

    /** 視点ワールド座標 (xyz=eye)。 */
    FVec4 camera_pos;

    /** 太陽方向 (xyz、camera→sun)。 */
    FVec4 sun_dir;

    /** 太陽の色 (xyz)。 */
    FVec4 sun_color;

    /** 太陽パラメータ (x=radius(1-cos), y=glow, zw=pad)。 */
    FVec4 sun_params;

    /** 天頂の色。 */
    FVec4 zenith;

    /** 地平線の色。 */
    FVec4 horizon;

    /** 地面方向の色。 */
    FVec4 ground;

    /** 雲パラメータ0 (x=coverage, y=density, z=time, w=enabled)。 */
    FVec4 cloud0;

    /** 雲パラメータ1 (xyz=cloud_color, w=wind_speed)。 */
    FVec4 cloud1;
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

/**
 * ゼロ長を避けてベクトルを正規化する。
 *
 * @param v 正規化するベクトル。
 * @return 正規化したベクトル。長さがほぼ 0 なら (0,1,0) を返す。
 */
ACS_FORCEINLINE FVec3 NormalizeSafe(FVec3 v) noexcept {
    const f32 len2 = v.x*v.x + v.y*v.y + v.z*v.z;
    if (len2 < 1e-12f) return FVec3{0, 1, 0};
    const f32 inv = 1.0f / Sqrt(len2);
    return { v.x * inv, v.y * inv, v.z * inv };
}

} // namespace

/** 太陽方向を正規化して保持する。 */
void FSky::SetSunDirection(FVec3 dir) noexcept { m_SunDir = NormalizeSafe(dir); }

/** VS/PS/定数バッファ/パイプラインを生成する。 */
TResult<void> FSky::Init(IRhiDevice& device, EFormat rt_format, EFormat depth_format) noexcept {
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kSkyHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "FSky.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    m_Vs = Move(vs_r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kSkyHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "FSky.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    m_Ps = Move(ps_r.Value());

    FBufferDesc cbd{};
    cbd.size = CBSize<SkyCB>();
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    auto cb_r = CreateRhiBuffer(device, cbd);
    if (cb_r.IsErr()) return Err<void>(cb_r.Error());
    m_Cb = Move(cb_r.Value());

    FPipelineDesc pd{};
    pd.vs = m_Vs.Get();
    pd.ps = m_Ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = depth_format;
    pd.depth_test    = false;          // sky は最初に塗るだけ。既存深度は維持
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 0;
    pd.cbuffer_names[0] = "FSky";
    pd.layout_count  = 0;
    pd.vertex_stride = 0;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    m_Pipeline = Move(pl_r.Value());

    return Ok();
}

/** GPU リソースを解放する。 */
void FSky::Shutdown() noexcept {
    m_Pipeline.Reset();
    m_Cb.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
}

/** 昼空プリセット (青空 + 白い太陽) を適用する。 */
void FSky::PresetDay() noexcept {
    m_SunDir     = NormalizeSafe(FVec3{0.4f, 0.7f, 0.4f});
    m_SunColor   = FVec3{1.0f, 0.95f, 0.85f};
    m_SunRadius  = 0.0006f;
    m_SunGlow    = 0.05f;
    m_Zenith      = FVec3{0.15f, 0.35f, 0.78f};
    m_Horizon     = FVec3{0.70f, 0.83f, 0.95f};
    m_Ground      = FVec3{0.18f, 0.20f, 0.20f};
    m_CloudColor    = FVec3{1.0f, 1.0f, 1.0f};
    m_CloudCoverage = 0.60f;
    m_CloudDensity  = 1.1f;
    m_CloudWind     = 1.0f;
}

/** 夕焼けプリセット (茜色 + 暖色太陽) を適用する。 */
void FSky::PresetSunset() noexcept {
    m_SunDir     = NormalizeSafe(FVec3{0.7f, 0.05f, 0.5f});
    m_SunColor   = FVec3{1.0f, 0.55f, 0.25f};
    m_SunRadius  = 0.001f;
    m_SunGlow    = 0.20f;
    m_Zenith      = FVec3{0.06f, 0.10f, 0.30f};
    m_Horizon     = FVec3{1.00f, 0.55f, 0.25f};
    m_Ground      = FVec3{0.10f, 0.06f, 0.08f};
    m_CloudColor    = FVec3{1.0f, 0.72f, 0.50f};   // 茜色に染まった雲
    m_CloudCoverage = 0.55f;
    m_CloudDensity  = 1.3f;
    m_CloudWind     = 0.8f;
}

/** 夜空プリセット (紺青 + 弱い月光) を適用する。 */
void FSky::PresetNight() noexcept {
    m_SunDir     = NormalizeSafe(FVec3{0.3f, 0.6f, 0.2f});
    m_SunColor   = FVec3{0.85f, 0.85f, 0.95f};
    m_SunRadius  = 0.0008f;
    m_SunGlow    = 0.04f;
    m_Zenith      = FVec3{0.02f, 0.03f, 0.08f};
    m_Horizon     = FVec3{0.05f, 0.07f, 0.15f};
    m_Ground      = FVec3{0.02f, 0.03f, 0.05f};
    m_CloudColor    = FVec3{0.10f, 0.12f, 0.20f};  // 紺青の薄い雲
    m_CloudCoverage = 0.38f;
    m_CloudDensity  = 1.8f;
    m_CloudWind     = 0.5f;
}

/** 定数バッファを更新し、フルスクリーン三角形でスカイを描画する。 */
void FSky::Render(IRhiCommandList& cl, const FCamera& camera) noexcept {
    if (!m_Pipeline || !m_Cb) return;
    SkyCB cb{};
    cb.inv_view_proj = Inverse(camera.ViewProjection());
    const FVec3 eye = camera.Eye();
    cb.camera_pos = FVec4{eye.x, eye.y, eye.z, 1};
    cb.sun_dir    = FVec4{m_SunDir.x, m_SunDir.y, m_SunDir.z, 0};
    cb.sun_color  = FVec4{m_SunColor.x, m_SunColor.y, m_SunColor.z, 1};
    cb.sun_params = FVec4{m_SunRadius, m_SunGlow, 0, 0};
    cb.zenith     = FVec4{m_Zenith.x, m_Zenith.y, m_Zenith.z, 1};
    cb.horizon    = FVec4{m_Horizon.x, m_Horizon.y, m_Horizon.z, 1};
    cb.ground     = FVec4{m_Ground.x, m_Ground.y, m_Ground.z, 1};
    cb.cloud0     = FVec4{m_CloudCoverage, m_CloudDensity, m_Time,
                          m_bCloudsEnabled ? 1.0f : 0.0f};
    cb.cloud1     = FVec4{m_CloudColor.x, m_CloudColor.y, m_CloudColor.z, m_CloudWind};
    m_Cb->Update(&cb, sizeof(cb));

    cl.SetPipeline(*m_Pipeline);
    cl.SetConstantBuffer(0, *m_Cb);
    cl.Draw(3);    // VB 無し、SV_VertexID で 3 頂点

    // 雲アニメ用の時間を内部で進める。これにより呼び出し側が SetTime を書かなくても
    // 雲が流れる (SetTime を毎フレーム呼べばそちらが優先され、決定論的に制御できる)。
    // 60fps 想定の固定ステップ。雲はゆっくり流れるので実フレームレート差は問題にならない。
    m_Time += 1.0f / 60.0f;
}

} // namespace acs
